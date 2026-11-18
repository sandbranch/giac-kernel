// End-to-end test: start giac-kernel the way a Jupyter client does (a
// connection file with a signing key), then talk to it over ZeroMQ.
//
// Usage: protocol-test PATH-TO-giac-kernel

#include "../src/wire.h"

#include <zmq.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using wire::json;
using wire::Message;

static int failures = 0;

#define CHECK(cond, what)                                                       \
	do {                                                                        \
		if (cond)                                                               \
			std::printf("ok    %s\n", what);                                    \
		else {                                                                  \
			std::printf("FAIL  %s  (%s:%d)\n", what, __FILE__, __LINE__);       \
			++failures;                                                         \
		}                                                                       \
	} while (0)

static int free_port(void *zctx)
{
	void *s = zmq_socket(zctx, ZMQ_ROUTER);
	zmq_bind(s, "tcp://127.0.0.1:*");
	char ep[256];
	size_t n = sizeof ep;
	zmq_getsockopt(s, ZMQ_LAST_ENDPOINT, ep, &n);
	zmq_close(s);
	return std::atoi(std::strrchr(ep, ':') + 1);
}

struct Client {
	void *zctx;
	wire::Session session{"a-test-key-0123"};
	void *shell, *control, *iopub, *hb;
	pid_t pid = -1;
	std::string unlink_path;

	explicit Client(const char *kernel_path) : zctx(zmq_ctx_new())
	{
		json conn = {{"transport", "tcp"},
		             {"ip", "127.0.0.1"},
		             {"key", "a-test-key-0123"},
		             {"signature_scheme", "hmac-sha256"},
		             {"shell_port", free_port(zctx)},
		             {"iopub_port", free_port(zctx)},
		             {"stdin_port", free_port(zctx)},
		             {"control_port", free_port(zctx)},
		             {"hb_port", free_port(zctx)}};
		char path[] = "/tmp/giac-kernel-test-XXXXXX.json";
		int fd = mkstemps(path, 5);
		std::string s = conn.dump();
		if (write(fd, s.data(), s.size()) != (ssize_t)s.size())
			std::abort();
		close(fd);
		pid = fork();
		if (pid == 0) {
			execl(kernel_path, kernel_path, path, (char *)nullptr);
			_exit(127);
		}
		auto ep = [&](const char *k) { return "tcp://127.0.0.1:" + std::to_string(conn[k].get<int>()); };
		shell = connect(ZMQ_DEALER, ep("shell_port"));
		control = connect(ZMQ_DEALER, ep("control_port"));
		hb = connect(ZMQ_REQ, ep("hb_port"));
		iopub = connect(ZMQ_SUB, ep("iopub_port"));
		zmq_setsockopt(iopub, ZMQ_SUBSCRIBE, "", 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the kernel bind
		unlink_path = path;
	}

	~Client()
	{
		for (void *s : {shell, control, iopub, hb})
			zmq_close(s);
		zmq_ctx_term(zctx);
		std::remove(unlink_path.c_str());
		if (pid > 0 && waitpid(pid, nullptr, WNOHANG) == 0) {
			kill(pid, SIGKILL);
			waitpid(pid, nullptr, 0);
		}
	}

	void *connect(int type, const std::string &ep)
	{
		void *s = zmq_socket(zctx, type);
		int linger = 0, timeout = 30000;
		zmq_setsockopt(s, ZMQ_LINGER, &linger, sizeof linger);
		zmq_setsockopt(s, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
		zmq_connect(s, ep.c_str());
		return s;
	}

	Message request(void *sock, const std::string &type, json content)
	{
		session.send(sock, type, std::move(content), nullptr, {});
		Message m;
		if (!session.recv(sock, m))
			m.header = {{"msg_type", "TIMEOUT"}};
		return m;
	}

	// All IOPub messages for one request, up to its status: idle.
	std::vector<Message> iopub_for(const Message &reply)
	{
		std::vector<Message> out;
		std::string parent = reply.parent_header.value("msg_id", "");
		for (;;) {
			Message m;
			if (!session.recv(iopub, m))
				break;
			if (m.parent_header.value("msg_id", "") != parent)
				continue;
			out.push_back(m);
			if (m.type() == "status" && m.content.value("execution_state", "") == "idle")
				break;
		}
		return out;
	}

	struct Run {
		Message reply;
		std::vector<Message> iopub;
		const Message *find(const std::string &type) const
		{
			for (auto &m : iopub)
				if (m.type() == type)
					return &m;
			return nullptr;
		}
		std::string result(const std::string &mime) const
		{
			auto *m = find("execute_result");
			return m && m->content["data"].contains(mime) ? m->content["data"][mime].get<std::string>() : "";
		}
	};

	Run execute(const std::string &code)
	{
		Run r;
		r.reply = request(shell, "execute_request",
		                  {{"code", code}, {"silent", false}, {"store_history", true},
		                   {"user_expressions", json::object()}, {"allow_stdin", false}});
		r.iopub = iopub_for(r.reply);
		return r;
	}
};

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: protocol-test PATH-TO-giac-kernel\n");
		return 2;
	}
	Client c(argv[1]);

	// heartbeat
	zmq_send(c.hb, "ping", 4, 0);
	char buf[8] = {};
	CHECK(zmq_recv(c.hb, buf, sizeof buf, 0) == 4 && std::string(buf, 4) == "ping", "heartbeat echoes");

	Message info = c.request(c.shell, "kernel_info_request", json::object());
	CHECK(info.type() == "kernel_info_reply", "kernel_info_reply arrives (signed both ways)");
	CHECK(info.content["language_info"]["name"] == "giac", "language is giac");
	c.iopub_for(info);

	auto r = c.execute("factor(x^4-1)");
	CHECK(r.reply.content["status"] == "ok", "factor: status ok");
	CHECK(r.result("text/plain") == "(x-1)*(x+1)*(x^2+1)", "factor: text/plain");
	CHECK(r.result("text/latex").find("\\left(x-1\\right)") != std::string::npos, "factor: text/latex");
	CHECK(r.reply.content["execution_count"] == 1, "execution_count starts at 1");

	r = c.execute("desolve(y''+y=0,y)");
	CHECK(r.result("text/plain") == "c_0*cos(x)+c_1*sin(x)", "desolve");

	r = c.execute("int(sin(x)/x,x,0,inf)");
	CHECK(r.result("text/plain") == "pi/2", "definite integral");

	r = c.execute("a:=3:;");
	CHECK(r.reply.content["status"] == "ok" && !r.find("execute_result"), "trailing :; hides the result");
	r = c.execute("a^2");
	CHECK(r.result("text/plain") == "9", "variables persist between cells");

	r = c.execute("print(\"hello\"); 7");
	auto *st = r.find("stream");
	CHECK(st && st->content["text"].get<std::string>().find("hello") != std::string::npos, "print goes to stdout stream");

	r = c.execute("a:=;");
	CHECK(r.reply.content["status"] == "error" && r.reply.content["ename"] == "SyntaxError", "syntax error reported");
	CHECK(r.find("error") != nullptr, "error published on iopub");

	r = c.execute("int(1,2,3,4,5)");
	CHECK(r.reply.content["status"] == "error" && r.reply.content["ename"] == "GiacError", "evaluation error reported");

	r = c.execute("plot(sin(x),x=0..pi)");
	CHECK(r.result("image/svg+xml").rfind("<svg", 0) == 0, "plot gives SVG");

	r = c.execute("?factor");
	st = r.find("stream");
	CHECK(st && st->content["text"].get<std::string>().find("Factorizes") != std::string::npos, "?factor shows help");

	Message comp = c.request(c.shell, "complete_request", {{"code", "y:=facto"}, {"cursor_pos", 8}});
	c.iopub_for(comp);
	bool has_factor = false;
	for (auto &m : comp.content["matches"])
		has_factor |= m == "factor";
	CHECK(has_factor && comp.content["cursor_start"] == 3 && comp.content["cursor_end"] == 8, "completion");

	Message insp = c.request(c.shell, "inspect_request", {{"code", "factor(x)"}, {"cursor_pos", 3}, {"detail_level", 0}});
	c.iopub_for(insp);
	CHECK(insp.content["found"] == true, "inspect finds help");

	Message ic = c.request(c.shell, "is_complete_request", {{"code", "f(x):={ x+1;"}});
	c.iopub_for(ic);
	CHECK(ic.content["status"] == "incomplete", "open block is incomplete");
	ic = c.request(c.shell, "is_complete_request", {{"code", "x+1"}});
	c.iopub_for(ic);
	CHECK(ic.content["status"] == "complete", "x+1 is complete");

	// interrupt a long loop through the control channel
	c.session.send(c.shell, "execute_request",
	               {{"code", "s:=0; for(k:=0;k<10^9;k++){s:=s+k;}; s"}, {"silent", false},
	                {"store_history", true}, {"user_expressions", json::object()}, {"allow_stdin", false}},
	               nullptr, {});
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	auto t0 = std::chrono::steady_clock::now();
	Message ir = c.request(c.control, "interrupt_request", json::object());
	CHECK(ir.type() == "interrupt_reply", "interrupt_reply on control while busy");
	Message er;
	c.session.recv(c.shell, er);
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	CHECK(er.content["status"] == "error" && er.content["ename"] == "Interrupted", "long loop interrupted");
	CHECK(secs < 5, "interrupt is prompt");
	c.iopub_for(er);
	r = c.execute("1+1");
	CHECK(r.result("text/plain") == "2", "kernel works after interrupt");

	// SIGINT, as jotter does with interrupt_mode "signal"
	c.session.send(c.shell, "execute_request",
	               {{"code", "s:=0; for(k:=0;k<10^9;k++){s:=s+k;}; s"}, {"silent", false},
	                {"store_history", true}, {"user_expressions", json::object()}, {"allow_stdin", false}},
	               nullptr, {});
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	kill(c.pid, SIGINT);
	c.session.recv(c.shell, er);
	CHECK(er.content["ename"] == "Interrupted", "SIGINT interrupts");
	c.iopub_for(er);

	Message sd = c.request(c.control, "shutdown_request", {{"restart", false}});
	CHECK(sd.type() == "shutdown_reply", "shutdown_reply");
	int status = -1;
	for (int i = 0; i < 50 && waitpid(c.pid, &status, WNOHANG) == 0; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "kernel exits cleanly after shutdown");
	c.pid = -1;

	std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
