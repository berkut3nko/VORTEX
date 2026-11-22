module;

export module vortex.net;

// docs/design/networking.md: the session protocol. GPU-free like vortex.sim (Vortex::Net links
// Sim and below only; vortex_assert_gpu_free checks it), with ENet behind EnetTransport (stage 5;
// linked privately, never into Core, Physics or Sim: vortex_assert_no_network). rendezvous.md [nat-R0]:
// STUN, the hashes, hole punching (Traversal) and the pair test, all GPU-free and socket-free but for
// EnetTransport's raw datagrams.
export import :transport;
export import :address;
export import :crypto;
export import :stun;
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
export import :bot;
export import :traversal;
export import :punch;
