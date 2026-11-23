module;

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

export module vortex.net:https;

// docs/design/rendezvous.md "The engine side": the HTTPS client net::Rendezvous talks to. Net has no
// HTTPS of its own (vortex_https implements it with libcurl; tests give a fake): start, poll, cancel.

export namespace vortex::net {

    struct HttpsRequest {
        std::string method = "GET";   ///< "GET", "POST" or "DELETE"
        std::string url;              ///< the whole URL
        std::string body;             ///< sent as is with Content-Type: application/json when not empty
        std::vector<std::pair<std::string, std::string>> headers;   ///< e.g. {"Authorization", "Bearer <hex>"}
        uint32_t timeoutMs = 10000;   ///< the whole transfer
        bool longPoll = false;        ///< runs on the long-poll thread (one at a time); else on the short-call thread
    };

    struct HttpsResponse {
        int status = 0;               ///< the HTTP status; 0 when failed
        std::string body;
        bool failed = false;          ///< no HTTP answer: DNS, TCP, TLS, timeout or cancelled
        std::string error;            ///< what failed ("cancelled" after Cancel)
    };

    using HttpsRequestId = uint32_t;  ///< 0 is never a request

    class HttpsClient {
    public:
        virtual ~HttpsClient() = default;
        /// @brief Queues the request and returns at once.
        virtual HttpsRequestId Start(HttpsRequest request) = 0;
        /// @brief The answer once it came (then forgotten: a second Take returns nullopt).
        virtual std::optional<HttpsResponse> Take(HttpsRequestId id) = 0;
        /// @brief Stops a queued or running request; its answer is {failed, "cancelled"}.
        virtual void Cancel(HttpsRequestId id) = 0;
    };
}
