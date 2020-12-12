#ifndef LOAD_BALANCE_SIMULATE
#define LOAD_BALANCE_SIMULATE
#include <vector>
#include <deque>
#include <memory>
#include <stdlib.h> 
#include <time.h> 
#include <string>
#include <cassert>
#include <iostream>
#include <fstream>
#include <algorithm>


// The number of request that can be processed by an upstream server at the same time.
constexpr int concurrency = 6;

struct ProxyServer;

static std::vector<int> response_latency;
static std::vector<int> request_proxy;
static std::vector<int> response_server;

struct UpstreamServer {
    UpstreamServer(int server_id_): server_id{server_id_} {
        service_time = 100;
    }
    void onReceiveRequest(int source) {
        service_timers.push_back(service_time);
        request_sources.push_back(source);
        latencies.push_back(0);
    }
    void processRequest() {
        for (int& latency: latencies) latency++;
        for (int i = 0; i < service_timers.size() && i < concurrency; i++) {
            service_timers[i]--;
        }
        while (!service_timers.empty() && service_timers.front() == 0) {
            service_timers.pop_front();
            response_latency.push_back(latencies.front());
            latencies.pop_front();
            request_proxy.push_back(request_sources.front());
            request_sources.pop_front();
            response_server.push_back(server_id);
        }
    }
    int activeRequests() {
        return service_timers.size();
    }
private:
    // Simulate time to process a request, a response will be sent to proxy when it has been processed.
    std::deque<int> service_timers; 
    // Proxy servers from which requests com from.
    std::deque<int> request_sources;
    // Total latency of a request.
    std::deque<int> latencies;
    // The unique id of the server.
    int server_id;
    // Time to process a request.
    int service_time;
};

// The cluster of upstream servers that actually serve the request.
struct Backend {
    Backend(int backend_servers) {
        for (int i = 0; i < backend_servers; i++) upstream_servers.emplace_back(i);
    }
    // Return total number of upstream servers.
    int num_servers() {
        return upstream_servers.size();
    }
    // Route the request to a specified upstream server.
    void onReceiveRequest(int upstream_server, int proxy) {
        upstream_servers[upstream_server].onReceiveRequest(proxy);
    }
    // Each upstream server process requests.
    void processRequest() {
        for (auto& server: upstream_servers) server.processRequest();
    }
    // return the number of active requests in each upstream server.
    std::vector<int> activeRequests() {
        std::vector<int> states;
        for (auto& server: upstream_servers) states.push_back(server.activeRequests());
        return states;
    } 
private:
    std::vector<UpstreamServer> upstream_servers;
};

struct LoadBalancer {
    LoadBalancer(Backend* backend_): active_requests(backend_->num_servers(), 0) {}
    virtual ~LoadBalancer() {};
    // Route a request to a backend server and return the index of the server routed to.
    virtual int selectUpstreamServer(int) = 0;
    // update the number of outstanding request of a server.
    void onSendRequest(const int upstream_server) {
        active_requests[upstream_server]++;
    }
    // Update the state of load balancer when a response is received.
    void onReceiveResponse(const int upstream_server) {
        active_requests[upstream_server]--;
    }
protected:
    std::vector<int> active_requests; 
    friend class ProxyServer;
};

struct RoundRobin: public LoadBalancer{
    RoundRobin(Backend* backend_): LoadBalancer(backend_) {}
    ~RoundRobin() override {}
    int selectUpstreamServer(int num_servers) override {
        cur_idx++;
        cur_idx %= num_servers;
        return cur_idx;
    }
private:
    int cur_idx = 0;
};

struct RandomSelect: public LoadBalancer{
    RandomSelect(Backend* backend_): LoadBalancer(backend_) {}
    ~RandomSelect() override {}
    int selectUpstreamServer(int num_servers) override {
        return rand() % num_servers; 
    }
};

// Use power of two choices, select 2 random backend hosts and pick the host with least active requests. 
struct LeastRequests: public LoadBalancer{
    LeastRequests(Backend* backend_): LoadBalancer(backend_) {
    }
    ~LeastRequests() override {}
    int selectUpstreamServer(int num_servers) override {
        int server1 = rand() % num_servers;
        int server2 = server1;
        while (server2 == server1) server2 = rand() % num_servers;
        return active_requests[server1] < active_requests[server2] ? server1 : server2;
    }
};


// A proxy server with configured load balancer.
struct ProxyServer {
    ProxyServer(Backend* backend_, std::string lb_policy_, int id_): backend{backend_}, id{id_}  {
        if (lb_policy_ == "Round Robin") load_balancer = std::make_unique<RoundRobin>(backend_);
        else if (lb_policy_ == "Random Select") load_balancer = std::make_unique<RandomSelect>(backend_);
        else if (lb_policy_ == "Least Request") load_balancer = std::make_unique<LeastRequests>(backend_);
    }
    void onSendRequest() {
        const int selected_upstream = load_balancer->selectUpstreamServer(backend->num_servers());
        load_balancer->onSendRequest(selected_upstream);
        backend->onReceiveRequest(selected_upstream, id);
    }
    void onReceiveResponse(int upstream_server) {
        load_balancer->onReceiveResponse(upstream_server);
    }
private:
    Backend* backend; 
    std::unique_ptr<LoadBalancer> load_balancer;
    int id;
};

// The cluster of proxy server for load balancing.
struct Frontend {
    Frontend(int proxy_servers_, Backend* backend_, std::string lb_policy_) {
        for (int i = 0; i < proxy_servers_; i++) proxies.emplace_back(backend_, lb_policy_, i);
    }
    int onReceiveRequest() { 
        for (auto& proxy: proxies) {
            if (rand() % proxies.size() == 0) proxy.onSendRequest();
        }
    }
private:
    std::vector<ProxyServer> proxies;
    friend class LBSimulator;
};

struct LBSimulator {
    LBSimulator(int proxy_servers_, int backend_servers_, std::string lb_policy_): backend(backend_servers_), frontend{proxy_servers_, &backend, lb_policy_} {
        cleanup();
        output.open(lb_policy_);
    }
    void run1TimeUnit() {
        backend.processRequest();
        collectStats();
        cleanup();
        frontend.onReceiveRequest();
    }
    void collectStats() {
        timer++;
        for (int i = 0; i < request_proxy.size(); i++) frontend.proxies[request_proxy[i]].onReceiveResponse(response_server[i]);
        request_cnt += response_latency.size();
        for (int latency: response_latency) {
            // output << latency << " ";
            total_latency += latency;
            all_latency.push_back(latency);
        }
        std::vector<int> states = backend.activeRequests();
        output << *std::max_element(states.begin(), states.end()) - *std::min_element(states.begin(), states.end()) << " ";
    }
    void cleanup() {
        request_proxy.clear();
        response_latency.clear();
        response_server.clear();
    }
    int latency() {
        std::sort(all_latency.begin(), all_latency.end());
        int tail = request_cnt / 1000;
        std::cout << all_latency[all_latency.size() - tail] << std::endl;
        return total_latency / request_cnt;
    }
private:
    Backend backend;
    Frontend frontend; 
    std::vector<std::vector<int>> backend_state;
    std::vector<int> all_latency;
    int request_cnt = 0;
    long long total_latency = 0;
    int timer = 0;
    std::ofstream output;
};

#endif
