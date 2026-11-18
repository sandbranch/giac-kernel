// Jupyter kernel wire protocol (version 5.3) over libzmq.
//
// A message on the wire is a ZeroMQ multipart message:
//   [routing ids...] "<IDS|MSG>" signature header parent_header metadata content
// The signature is HMAC-SHA256 over the four JSON frames, hex encoded, keyed
// with the "key" from the connection file. An empty key disables signing.
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace wire {

using json = nlohmann::json;

struct Connection {
	std::string transport, ip, key, signature_scheme;
	int shell_port = 0, iopub_port = 0, stdin_port = 0, control_port = 0, hb_port = 0;

	static Connection load(const std::string &path);
	std::string endpoint(int port) const;
};

struct Message {
	std::vector<std::string> ids;  // routing identities, echoed back on replies
	json header, parent_header, metadata, content;

	std::string type() const { return header.value("msg_type", ""); }
};

class Session {
public:
	explicit Session(std::string key);

	// Receive one message. Returns false on a bad signature or malformed
	// framing (the message is dropped) or when the socket is shut down.
	bool recv(void *socket, Message &out) const;

	// Send a message of type msg_type answering parent (may be null).
	// ids are the routing frames: the requester's on ROUTER sockets, a topic
	// on IOPub.
	void send(void *socket, const std::string &msg_type, json content,
	          const Message *parent, const std::vector<std::string> &ids) const;

	const std::string &id() const { return session_id_; }

private:
	std::string sign(const std::string &h, const std::string &p,
	                 const std::string &m, const std::string &c) const;

	std::string key_;
	std::string session_id_;
};

std::string new_uuid();
std::string utc_now_iso8601();

}  // namespace wire
