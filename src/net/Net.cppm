module;

export module vortex.net;

// docs/design/networking.md: the session protocol. GPU-free like vortex.sim (Vortex::Net links
// Sim and below only; vortex_assert_gpu_free checks it). Stages 3-4: loopback transports only.
export import :transport;
export import :wire;
export import :messages;
export import :prediction;
export import :session;
export import :server;
export import :client;
export import :drill;
