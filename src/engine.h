// The Giac side of the kernel: everything that touches libgiac lives here,
// so the protocol code never includes Giac's headers.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct EvalResult {
	std::string output;  // text printed while evaluating (print(), warnings)
	bool ok = true;
	std::string ename, evalue;  // set when !ok
	// MIME type -> representation. Empty when there is nothing to show
	// (input ended in ":;", or the result is a plain "Done").
	std::vector<std::pair<std::string, std::string>> data;
};

struct Completion {
	std::vector<std::string> matches;
	size_t start = 0, end = 0;  // byte offsets of the word being completed
};

class Engine {
public:
	Engine();
	~Engine();

	EvalResult execute(const std::string &code);

	// "complete", "incomplete" or "invalid", as is_complete_reply wants.
	std::string is_complete(const std::string &code);

	Completion complete(const std::string &code, size_t cursor_byte);

	// Help text for the identifier at or before cursor_byte; empty if none.
	std::string help_at(const std::string &code, size_t cursor_byte);
	std::string help(const std::string &name);

	std::string version() const;

	// Ask a running evaluation to stop. Async-signal-safe: only sets Giac's
	// interrupt flag, which Giac polls while it computes.
	static void interrupt();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
