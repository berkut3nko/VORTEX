module;

#include <memory>

export module vortex.https;

import vortex.net;

// docs/design/rendezvous.md "The engine side": net::HttpsClient over libcurl, on two threads (the long
// poll alone on one, the short calls on the other). Linked only by vortex_server and the game.

export namespace vortex::https {
    /// @brief A client whose requests never block the caller: Start queues, Take polls, Cancel aborts.
    std::unique_ptr<net::HttpsClient> MakeCurlHttpsClient();
}
