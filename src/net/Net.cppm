module;

export module vortex.net;

// docs/design/networking.md: the session protocol. GPU-free like vortex.sim (Vortex::Net links
// Sim and below only; vortex_assert_gpu_free checks it), with ENet behind EnetTransport (stage 5;
// linked privately, never into Core, Physics or Sim: vortex_assert_no_network).
export import :transport;
export import :enet;
export import :wire;
export import :messages;
export import :prediction;
export import :session;
export import :entities;
export import :sync;
export import :server;
export import :client;
export import :drill;
