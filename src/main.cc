// giac-kernel: a Jupyter kernel for the Giac computer algebra system.
//
// Started by a Jupyter client (jotter, JupyterLab, ...) as
//   giac-kernel <connection-file>
// Threads: the main thread serves the shell socket and evaluates Giac code;
// one thread echoes heartbeats; one serves the control socket, so interrupt
// and shutdown requests get through while a long computation runs.

#include "engine.h"
#include "wire.h"

#include <zmq.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef GIAC_KERNEL_VERSION
#define GIAC_KERNEL_VERSION "0.0.0"
#endif

using wire::json;
using wire::Message;

static std::atomic<bool> g_quit{false};

static void on_sigint(int) { Engine::interrupt(); }

// Jupyter counts cursor positions in Unicode code points; we work in bytes.
static size_t codepoints_to_bytes(const std::string &s, long cp)
{
	size_t i = 0;
	for (long n = 0; i < s.size() && n < cp; ++n)
		do
			++i;
		while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80);
	return i;
}

static long bytes_to_codepoints(const std::string &s, size_t b)
{
	long n = 0;
	for (size_t i = 0; i < b && i < s.size(); ++i)
		if (((unsigned char)s[i] & 0xC0) != 0x80)
			++n;
	return n;
}

class Kernel {
public:
	Kernel(const wire::Connection &conn)
	    : conn_(conn), session_(conn.key), zctx_(zmq_ctx_new())
	{
		shell_ = bind(ZMQ_ROUTER, conn.shell_port);
		stdin_ = bind(ZMQ_ROUTER, conn.stdin_port);
		iopub_ = bind(ZMQ_PUB, conn.iopub_port);
		kernel_info_ = make_kernel_info();
		// The other two sockets are created and used only by their threads.
		hb_thread_ = std::thread([this] { heartbeat(); });
		control_thread_ = std::thread([this] { control(); });
	}

	~Kernel()
	{
		g_quit = true;
		zmq_ctx_shutdown(zctx_);  // wakes the threads blocked in recv
		hb_thread_.join();
		control_thread_.join();
		for (void *s : {shell_, stdin_, iopub_})
			zmq_close(s);
		zmq_ctx_term(zctx_);
	}

	void run()
	{
		while (!g_quit) {
			zmq_pollitem_t item = {shell_, 0, ZMQ_POLLIN, 0};
			int rc = zmq_poll(&item, 1, 200);
			if (rc < 0) {
				if (errno == EINTR)
					continue;  // SIGINT while idle
				break;
			}
			if (rc == 0)
				continue;
			Message m;
			if (session_.recv(shell_, m))
				on_shell(m);
		}
	}

private:
	void *bind(int type, int port)
	{
		void *s = zmq_socket(zctx_, type);
		int linger = 1000;
		zmq_setsockopt(s, ZMQ_LINGER, &linger, sizeof linger);
		std::string ep = conn_.endpoint(port);
		if (zmq_bind(s, ep.c_str()) != 0)
			throw std::runtime_error("cannot bind " + ep + ": " + zmq_strerror(errno));
		return s;
	}

	json make_kernel_info()
	{
		std::string gv = engine_.version();
		return {{"status", "ok"},
		        {"protocol_version", "5.3"},
		        {"implementation", "giac-kernel"},
		        {"implementation_version", GIAC_KERNEL_VERSION},
		        {"language_info",
		         {{"name", "giac"},
		          {"version", gv},
		          {"mimetype", "text/x-giac"},
		          {"file_extension", ".giac"},
		          // no Giac grammar exists in front ends; MATLAB's is the closest
		          // (name(args), [lists], keywords, and x' read as an operator)
		          {"pygments_lexer", "matlab"},
		          {"codemirror_mode", "octave"}}},
		        {"banner", "Giac " + gv + " computer algebra system\n"
		                   "giac-kernel " GIAC_KERNEL_VERSION ". Help on a command: ?factor"},
		        {"help_links",
		         json::array({{{"text", "Giac/Xcas documentation"},
		                       {"url", "https://www-fourier.univ-grenoble-alpes.fr/~parisse/giac.html"}}})}};
	}

	// IOPub messages carry a topic frame in place of routing ids.
	void publish(const std::string &type, json content, const Message &parent)
	{
		session_.send(iopub_, type, std::move(content), &parent, {"kernel." + session_.id() + "." + type});
	}

	void reply(void *socket, const std::string &type, json content, const Message &req)
	{
		session_.send(socket, type, std::move(content), &req, req.ids);
	}

	void on_shell(const Message &m)
	{
		const std::string t = m.type();
		publish("status", {{"execution_state", "busy"}}, m);
		if (t == "kernel_info_request")
			reply(shell_, "kernel_info_reply", kernel_info_, m);
		else if (t == "execute_request")
			on_execute(m);
		else if (t == "complete_request")
			on_complete(m);
		else if (t == "inspect_request")
			on_inspect(m);
		else if (t == "is_complete_request")
			on_is_complete(m);
		else if (t == "history_request")
			reply(shell_, "history_reply", {{"status", "ok"}, {"history", json::array()}}, m);
		else if (t == "comm_info_request")
			reply(shell_, "comm_info_reply", {{"status", "ok"}, {"comms", json::object()}}, m);
		else if (t == "shutdown_request") {  // older clients send it on shell
			reply(shell_, "shutdown_reply", shutdown_content(m), m);
			g_quit = true;
		}
		publish("status", {{"execution_state", "idle"}}, m);
	}

	void on_execute(const Message &m)
	{
		std::string code = m.content.value("code", "");
		bool silent = m.content.value("silent", false);
		bool store = m.content.value("store_history", true) && !silent;
		if (store)
			++count_;
		if (!silent)
			publish("execute_input", {{"code", code}, {"execution_count", count_}}, m);

		EvalResult r = engine_.execute(code);

		if (!silent && !r.output.empty())
			publish("stream", {{"name", "stdout"}, {"text", r.output}}, m);
		if (!r.ok) {
			json err = {{"ename", r.ename},
			            {"evalue", r.evalue},
			            {"traceback", json::array({r.ename + ": " + r.evalue})}};
			if (!silent)
				publish("error", err, m);
			err["status"] = "error";
			err["execution_count"] = count_;
			reply(shell_, "execute_reply", err, m);
			return;
		}
		if (!silent && !r.data.empty()) {
			json data = json::object();
			for (auto &kv : r.data)
				data[kv.first] = kv.second;
			publish("execute_result",
			        {{"execution_count", count_}, {"data", data}, {"metadata", json::object()}}, m);
		}
		reply(shell_, "execute_reply",
		      {{"status", "ok"},
		       {"execution_count", count_},
		       {"user_expressions", json::object()},
		       {"payload", json::array()}},
		      m);
	}

	void on_complete(const Message &m)
	{
		std::string code = m.content.value("code", "");
		long cp = m.content.value("cursor_pos", (long)0);
		Completion c = engine_.complete(code, codepoints_to_bytes(code, cp));
		reply(shell_, "complete_reply",
		      {{"status", "ok"},
		       {"matches", c.matches},
		       {"cursor_start", bytes_to_codepoints(code, c.start)},
		       {"cursor_end", bytes_to_codepoints(code, c.end)},
		       {"metadata", json::object()}},
		      m);
	}

	void on_inspect(const Message &m)
	{
		std::string code = m.content.value("code", "");
		long cp = m.content.value("cursor_pos", (long)0);
		std::string h = engine_.help_at(code, codepoints_to_bytes(code, cp));
		json data = json::object();
		if (!h.empty())
			data["text/plain"] = h;
		reply(shell_, "inspect_reply",
		      {{"status", "ok"}, {"found", !h.empty()}, {"data", data}, {"metadata", json::object()}}, m);
	}

	void on_is_complete(const Message &m)
	{
		std::string status = engine_.is_complete(m.content.value("code", ""));
		json c = {{"status", status}};
		if (status == "incomplete")
			c["indent"] = "";
		reply(shell_, "is_complete_reply", c, m);
	}

	static json shutdown_content(const Message &m)
	{
		return {{"status", "ok"}, {"restart", m.content.value("restart", false)}};
	}

	void heartbeat()
	{
		void *hb = zmq_socket(zctx_, ZMQ_REP);
		int linger = 0;
		zmq_setsockopt(hb, ZMQ_LINGER, &linger, sizeof linger);
		if (zmq_bind(hb, conn_.endpoint(conn_.hb_port).c_str()) == 0) {
			char buf[256];
			for (;;) {
				int n = zmq_recv(hb, buf, sizeof buf, 0);
				if (n < 0) {
					if (errno == EINTR)
						continue;
					break;  // ETERM: shutting down
				}
				zmq_send(hb, buf, std::min<size_t>(n, sizeof buf), 0);
			}
		}
		zmq_close(hb);
	}

	void control()
	{
		void *ctl = zmq_socket(zctx_, ZMQ_ROUTER);
		int linger = 1000;
		zmq_setsockopt(ctl, ZMQ_LINGER, &linger, sizeof linger);
		if (zmq_bind(ctl, conn_.endpoint(conn_.control_port).c_str()) == 0) {
			for (;;) {
				Message m;
				if (!session_.recv(ctl, m)) {
					if (errno == ETERM)
						break;
					continue;  // bad signature or EINTR
				}
				std::string t = m.type();
				if (t == "interrupt_request") {
					Engine::interrupt();
					reply(ctl, "interrupt_reply", {{"status", "ok"}}, m);
				} else if (t == "shutdown_request") {
					reply(ctl, "shutdown_reply", shutdown_content(m), m);
					g_quit = true;
					Engine::interrupt();  // stop a running evaluation
				} else if (t == "kernel_info_request") {
					reply(ctl, "kernel_info_reply", kernel_info_, m);
				}
			}
		}
		zmq_close(ctl);
	}

	wire::Connection conn_;
	wire::Session session_;
	Engine engine_;
	void *zctx_;
	void *shell_ = nullptr, *stdin_ = nullptr, *iopub_ = nullptr;
	json kernel_info_;
	long count_ = 0;
	std::thread hb_thread_, control_thread_;
};

int main(int argc, char **argv)
{
	if (argc == 2 && (!std::strcmp(argv[1], "--version") || !std::strcmp(argv[1], "-V"))) {
		std::printf("giac-kernel %s\n", GIAC_KERNEL_VERSION);
		return 0;
	}
	if (argc != 2 || argv[1][0] == '-') {
		std::fprintf(stderr,
		             "usage: giac-kernel CONNECTION_FILE\n"
		             "A Jupyter kernel for Giac. Jupyter clients such as jotter start it\n"
		             "through its kernelspec (kernel name \"giac\").\n");
		return argc == 2 && !std::strcmp(argv[1], "--help") ? 0 : 2;
	}

	struct sigaction sa {};
	sa.sa_handler = on_sigint;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, nullptr);

	try {
		wire::Connection conn = wire::Connection::load(argv[1]);
		Kernel k(conn);
		k.run();
	} catch (const std::exception &e) {
		std::fprintf(stderr, "giac-kernel: %s\n", e.what());
		return 1;
	}
	return 0;
}
