module;

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

module vortex.https;

import vortex.net;

// docs/design/rendezvous.md "The engine side" (vortex_https): two lanes, each a thread with a queue. The
// long poll runs alone on its lane, so a register, a join or the DELETE never waits behind it. A
// cancelled request aborts at its next progress call (curl makes one at least once a second).

namespace vortex::https {

    namespace {
        std::once_flag g_Init;

        using Flag = std::shared_ptr<std::atomic<bool>>;

        struct Job {
            net::HttpsRequestId id = 0;
            net::HttpsRequest request;
            Flag cancel;
        };

        struct Lane {
            std::mutex mutex;
            std::condition_variable wake;
            std::deque<Job> queue;
            std::thread thread;
        };

        size_t Append(char* data, size_t size, size_t count, void* user) {
            static_cast<std::string*>(user)->append(data, size * count);
            return size * count;
        }

        int Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            return static_cast<std::atomic<bool>*>(user)->load() ? 1 : 0;
        }

        net::HttpsResponse Failed(std::string error) {
            net::HttpsResponse r;
            r.failed = true;
            r.error = std::move(error);
            return r;
        }

        net::HttpsResponse Perform(Job& job) {
            CURL* curl = curl_easy_init();
            if (curl == nullptr) return Failed("curl_easy_init failed");
            const net::HttpsRequest& q = job.request;
            curl_slist* headers = nullptr;
            for (const auto& [name, value] : q.headers) headers = curl_slist_append(headers, (name + ": " + value).c_str());
            if (!q.body.empty()) headers = curl_slist_append(headers, "Content-Type: application/json");

            std::string body;
            char errorBuffer[CURL_ERROR_SIZE] = {};
            curl_easy_setopt(curl, CURLOPT_URL, q.url.c_str());
            if (q.method == "POST" || q.method == "DELETE") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, q.method.c_str());
            if (!q.body.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, q.body.c_str());   // not copied: job.request lives until the end
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(q.body.size()));
            } else if (q.method == "POST") {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
            }
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(q.timeoutMs));
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min<uint32_t>(q.timeoutMs, 5000)));
            curl_easy_setopt(curl, CURLOPT_IPRESOLVE, static_cast<long>(CURL_IPRESOLVE_V4));
            curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "vortex-rendezvous/1");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Append);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, job.cancel.get());
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);

            const CURLcode code = curl_easy_perform(curl);
            net::HttpsResponse r;
            if (code == CURLE_OK) {
                long status = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
                r.status = static_cast<int>(status);
                r.body = std::move(body);
            } else if (code == CURLE_ABORTED_BY_CALLBACK) {
                r = Failed("cancelled");
            } else {
                r = Failed(errorBuffer[0] != '\0' ? std::string(errorBuffer) : std::string(curl_easy_strerror(code)));
            }
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return r;
        }

        class CurlHttpsClient final : public net::HttpsClient {
        public:
            CurlHttpsClient() {
                m_Long.thread = std::thread([this] { Run(m_Long); });
                m_Short.thread = std::thread([this] { Run(m_Short); });
            }

            ~CurlHttpsClient() override {
                m_Stop = true;
                {
                    std::lock_guard lock(m_Mutex);
                    for (auto& [id, flag] : m_Cancels) flag->store(true);
                }
                for (Lane* lane : {&m_Long, &m_Short}) {
                    std::lock_guard lock(lane->mutex);
                    lane->wake.notify_all();
                }
                m_Long.thread.join();
                m_Short.thread.join();
            }

            net::HttpsRequestId Start(net::HttpsRequest request) override {
                Job job;
                {
                    std::lock_guard lock(m_Mutex);
                    job.id = ++m_Next;
                    job.cancel = std::make_shared<std::atomic<bool>>(false);
                    m_Cancels[job.id] = job.cancel;
                }
                const net::HttpsRequestId id = job.id;
                Lane& lane = request.longPoll ? m_Long : m_Short;
                job.request = std::move(request);
                {
                    std::lock_guard lock(lane.mutex);
                    lane.queue.push_back(std::move(job));
                }
                lane.wake.notify_one();
                return id;
            }

            std::optional<net::HttpsResponse> Take(net::HttpsRequestId id) override {
                std::lock_guard lock(m_Mutex);
                const auto it = m_Results.find(id);
                if (it == m_Results.end()) return std::nullopt;
                net::HttpsResponse r = std::move(it->second);
                m_Results.erase(it);
                m_Cancels.erase(id);
                return r;
            }

            void Cancel(net::HttpsRequestId id) override {
                std::lock_guard lock(m_Mutex);
                if (const auto it = m_Cancels.find(id); it != m_Cancels.end()) it->second->store(true);
            }

        private:
            void Run(Lane& lane) {
                for (;;) {
                    Job job;
                    {
                        std::unique_lock lock(lane.mutex);
                        lane.wake.wait(lock, [&] { return m_Stop.load() || !lane.queue.empty(); });
                        if (lane.queue.empty()) return;   // m_Stop and nothing left
                        job = std::move(lane.queue.front());
                        lane.queue.pop_front();
                    }
                    net::HttpsResponse response = job.cancel->load() ? Failed("cancelled") : Perform(job);
                    std::lock_guard lock(m_Mutex);
                    if (m_Cancels.contains(job.id)) m_Results[job.id] = std::move(response);
                }
            }

            Lane m_Long, m_Short;
            std::mutex m_Mutex;   ///< guards m_Results, m_Cancels, m_Next
            std::map<net::HttpsRequestId, net::HttpsResponse> m_Results;
            std::map<net::HttpsRequestId, Flag> m_Cancels;
            net::HttpsRequestId m_Next = 0;
            std::atomic<bool> m_Stop{false};
        };
    }

    std::unique_ptr<net::HttpsClient> MakeCurlHttpsClient() {
        std::call_once(g_Init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        return std::make_unique<CurlHttpsClient>();
    }
}
