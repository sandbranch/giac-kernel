#include "wire.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <zmq.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <stdexcept>

namespace wire {

static const std::string DELIMITER = "<IDS|MSG>";

Connection Connection::load(const std::string &path)
{
	std::ifstream in(path);
	if (!in)
		throw std::runtime_error("cannot open connection file " + path);
	json j = json::parse(in);
	Connection c;
	c.transport = j.value("transport", "tcp");
	c.ip = j.value("ip", "127.0.0.1");
	c.key = j.value("key", "");
	c.signature_scheme = j.value("signature_scheme", "hmac-sha256");
	c.shell_port = j.at("shell_port").get<int>();
	c.iopub_port = j.at("iopub_port").get<int>();
	c.stdin_port = j.at("stdin_port").get<int>();
	c.control_port = j.at("control_port").get<int>();
	c.hb_port = j.at("hb_port").get<int>();
	if (!c.key.empty() && c.signature_scheme != "hmac-sha256")
		throw std::runtime_error("unsupported signature scheme " + c.signature_scheme);
	return c;
}

std::string Connection::endpoint(int port) const
{
	// ipc endpoints are "<ip>-<port>" paths; tcp is "ip:port"
	if (transport == "ipc")
		return "ipc://" + ip + "-" + std::to_string(port);
	return transport + "://" + ip + ":" + std::to_string(port);
}

std::string new_uuid()
{
	static thread_local std::mt19937_64 rng{std::random_device{}()};
	unsigned char b[16];
	for (int i = 0; i < 16; i += 8) {
		uint64_t r = rng();
		for (int k = 0; k < 8; ++k)
			b[i + k] = (unsigned char)(r >> (8 * k));
	}
	b[6] = (b[6] & 0x0f) | 0x40;  // version 4
	b[8] = (b[8] & 0x3f) | 0x80;  // RFC 4122 variant
	char s[37];
	std::snprintf(s, sizeof s,
	              "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	              b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
	              b[11], b[12], b[13], b[14], b[15]);
	return s;
}

std::string utc_now_iso8601()
{
	using namespace std::chrono;
	auto now = system_clock::now();
	std::time_t t = system_clock::to_time_t(now);
	auto us = duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000;
	std::tm tm{};
	gmtime_r(&t, &tm);
	char s[64];
	std::snprintf(s, sizeof s, "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ", tm.tm_year + 1900,
	              tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (long)us);
	return s;
}

Session::Session(std::string key) : key_(std::move(key)), session_id_(new_uuid()) {}

std::string Session::sign(const std::string &h, const std::string &p,
                          const std::string &m, const std::string &c) const
{
	if (key_.empty())
		return "";
	unsigned char mac[EVP_MAX_MD_SIZE];
	EVP_MAC *alg = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
	EVP_MAC_CTX *mc = EVP_MAC_CTX_new(alg);
	char digest[] = "SHA256";
	OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string("digest", digest, 0),
	                       OSSL_PARAM_construct_end()};
	size_t out = 0;
	bool ok = mc && EVP_MAC_init(mc, (const unsigned char *)key_.data(), key_.size(), params) &&
	          EVP_MAC_update(mc, (const unsigned char *)h.data(), h.size()) &&
	          EVP_MAC_update(mc, (const unsigned char *)p.data(), p.size()) &&
	          EVP_MAC_update(mc, (const unsigned char *)m.data(), m.size()) &&
	          EVP_MAC_update(mc, (const unsigned char *)c.data(), c.size()) &&
	          EVP_MAC_final(mc, mac, &out, sizeof mac);
	EVP_MAC_CTX_free(mc);
	EVP_MAC_free(alg);
	if (!ok)
		throw std::runtime_error("HMAC failed");
	static const char hex[] = "0123456789abcdef";
	std::string s(2 * out, '0');
	for (size_t i = 0; i < out; ++i) {
		s[2 * i] = hex[mac[i] >> 4];
		s[2 * i + 1] = hex[mac[i] & 15];
	}
	return s;
}

static bool recv_multipart(void *socket, std::vector<std::string> &frames)
{
	frames.clear();
	for (;;) {
		zmq_msg_t part;
		zmq_msg_init(&part);
		if (zmq_msg_recv(&part, socket, 0) < 0) {
			zmq_msg_close(&part);
			return false;
		}
		frames.emplace_back((const char *)zmq_msg_data(&part), zmq_msg_size(&part));
		int more = zmq_msg_more(&part);
		zmq_msg_close(&part);
		if (!more)
			return true;
	}
}

bool Session::recv(void *socket, Message &out) const
{
	std::vector<std::string> f;
	if (!recv_multipart(socket, f))
		return false;
	size_t d = 0;
	while (d < f.size() && f[d] != DELIMITER)
		++d;
	if (f.size() < d + 6)  // delimiter, signature and four JSON frames
		return false;
	const std::string &sig = f[d + 1];
	if (!key_.empty()) {
		std::string want = sign(f[d + 2], f[d + 3], f[d + 4], f[d + 5]);
		if (sig.size() != want.size() || CRYPTO_memcmp(sig.data(), want.data(), want.size()) != 0)
			return false;
	}
	try {
		out.ids.assign(f.begin(), f.begin() + d);
		out.header = json::parse(f[d + 2]);
		out.parent_header = json::parse(f[d + 3]);
		out.metadata = json::parse(f[d + 4]);
		out.content = json::parse(f[d + 5]);
	} catch (const json::exception &) {
		return false;
	}
	return true;
}

void Session::send(void *socket, const std::string &msg_type, json content,
                   const Message *parent, const std::vector<std::string> &ids) const
{
	json header = {{"msg_id", new_uuid()},
	               {"session", session_id_},
	               {"username", "giac-kernel"},
	               {"date", utc_now_iso8601()},
	               {"msg_type", msg_type},
	               {"version", "5.3"}};
	std::string h = header.dump();
	std::string p = parent ? parent->header.dump() : "{}";
	std::string m = "{}";
	// Giac output can contain bytes that are not valid UTF-8; replace them
	// instead of throwing.
	std::string c = content.dump(-1, ' ', false, json::error_handler_t::replace);
	std::string sig = sign(h, p, m, c);

	std::vector<const std::string *> frames;
	for (const auto &id : ids)
		frames.push_back(&id);
	frames.push_back(&DELIMITER);
	for (const std::string *s : {&sig, &h, &p, &m, &c})
		frames.push_back(s);
	for (size_t i = 0; i < frames.size(); ++i)
		zmq_send(socket, frames[i]->data(), frames[i]->size(),
		         i + 1 < frames.size() ? ZMQ_SNDMORE : 0);
}

}  // namespace wire
