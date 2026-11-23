// docs/design/rendezvous.md [nat-R1]: a net::HttpsClient for tests. Nothing is sent: a test reads the
// requests the code under test started and answers them (Respond / Fail) when it chooses.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import vortex.net;

namespace fakehttps {
    using namespace vortex;

    struct FakeRequest {
        net::HttpsRequestId id = 0;
        net::HttpsRequest request;
        bool cancelled = false, taken = false;
        std::optional<net::HttpsResponse> response;
    };

    class FakeHttpsClient final : public net::HttpsClient {
    public:
        std::vector<FakeRequest> requests;   // in Start order

        net::HttpsRequestId Start(net::HttpsRequest r) override {
            FakeRequest q;
            q.id = ++next;
            q.request = std::move(r);
            requests.push_back(std::move(q));
            return next;
        }
        std::optional<net::HttpsResponse> Take(net::HttpsRequestId id) override {
            FakeRequest* q = Find(id);
            if (q == nullptr || q->taken || !q->response) return std::nullopt;
            q->taken = true;
            return q->response;
        }
        void Cancel(net::HttpsRequestId id) override {
            FakeRequest* q = Find(id);
            if (q == nullptr || q->response) return;
            q->cancelled = true;
            q->response = net::HttpsResponse{0, "", true, "cancelled"};
        }
        void Respond(net::HttpsRequestId id, int status, std::string body) {
            if (FakeRequest* q = Find(id)) q->response = net::HttpsResponse{status, std::move(body), false, ""};
        }
        void Fail(net::HttpsRequestId id, std::string error) {
            if (FakeRequest* q = Find(id)) q->response = net::HttpsResponse{0, "", true, std::move(error)};
        }
        FakeRequest* Last() { return requests.empty() ? nullptr : &requests.back(); }
        /// The request with this id (ids start at 1, in Start order).
        FakeRequest& At(net::HttpsRequestId id) { return requests.at(id - 1); }
        std::string Header(const FakeRequest& q, std::string_view name) const {
            for (const auto& [k, v] : q.request.headers)
                if (k == name) return v;
            return {};
        }

    private:
        FakeRequest* Find(net::HttpsRequestId id) {
            if (id == 0 || id > requests.size()) return nullptr;
            return &requests[id - 1];
        }
        net::HttpsRequestId next = 0;
    };
}
