// The add()/worker handshake against a store-buffer model, read from scheduler.h itself.
//
// A lost wake-up needs the wheel insert to stay invisible to the worker across a whole pass of the
// worker, which no run on arm64 or x86 shows (audit S1: none in 26.6 M stress adds), so no runtime
// test can tell a correct add() from one whose fence sits too late. This test checks the order of
// the operations instead. It reads the producer side of add() out of scheduler.h (the wheel insert,
// the fence, the loads of the published wakeup time and of `sleeping`, the compare-exchange that
// lowers it), runs it against the worker's pass in every interleaving of an x86-TSO machine
// (each thread's plain stores wait in a FIFO buffer; a fence or an RMW drains the thread's own
// buffer; a buffer drains to memory at any point), and fails if any execution leaves the worker
// asleep past the task with no signal sent. It also runs the order add() had before the fence moved
// (loads before the fence), which the model must find losing a task, so the model stays live.
//
// TSO is stronger than the C++ model and than arm64; a loss under TSO is a loss everywhere. The
// argument for the fixed order under C++ is the fence rule: of the two seq_cst fences, whichever
// comes first in the total order makes the other side see what preceded it (ARCHITECTURE.md).
//
//   scheduler_handshake_model PATH/TO/scheduler.h
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

enum Op { PUT, FENCE, LOAD_A, CAS_A, LOAD_S };
static const char* op_name(Op o) {
	static const char* n[] = {"put", "fence", "load(next_wakeup)", "cas(next_wakeup)", "load(sleeping)"};
	return n[o];
}

// Times, in arbitrary units: the producer's clock, the task's due time and the idle target the worker
// raises the published time to. The published time the worker woke early from (`stale`) is each of
// past, between now and the task, and after the task.
constexpr int kNow = 10, kDue = 30, kIdle = 100;
static int stale = 20;
enum Var { W, A, S, NVARS };   // W: the task is on the wheel; A: published wakeup; S: sleeping

struct Store { Var v; int x; };
struct State {
	int mem[NVARS] = {0, stale, 0};
	std::vector<Store> buf[2];   // 0: producer, 1: worker
	int pc[2] = {0, 0};
	int next = 0, sleeping = 0;  // the producer's locals
	bool signalled = false;
	int wlocal = stale, target = kIdle, n = 0;  // the worker's locals
};

static int load(const State& s, int t, Var v) {
	for (auto it = s.buf[t].rbegin(); it != s.buf[t].rend(); ++it) {
		if (it->v == v) {
			return it->x;
		}
	}
	return s.mem[v];
}
static void drain(State& s, int t) {
	for (auto& st : s.buf[t]) {
		s.mem[st.v] = st.x;
	}
	s.buf[t].clear();
}

// The worker's pass after an early wake: peep, raise the published time by compare-exchange,
// store `sleeping`, fence, reload the published time, peep again, sleep to the result.
constexpr int kWorkerSteps = 6;
static void worker_step(State& s) {
	switch (s.pc[1]++) {
		case 0: s.target = load(s, 1, W) ? kDue : kIdle; break;   // first peep
		case 1:                                                    // compare_exchange_strong(next, target)
			drain(s, 1);
			if (s.mem[A] == s.wlocal) {
				s.mem[A] = s.target;
			} else {
				s.wlocal = s.mem[A];
				if (s.mem[A] > s.target) {
					s.mem[A] = s.target;   // the lowering loop
				}
			}
			break;
		case 2: s.buf[1].push_back({S, 1}); break;
		case 3: drain(s, 1); break;
		case 4: s.n = load(s, 1, A); break;
		case 5:                                                    // late peep
			if (load(s, 1, W) && kDue < s.n) {
				s.n = kDue;
			}
			break;
	}
}

static void producer_step(State& s, const std::vector<Op>& prog) {
	switch (prog[s.pc[0]++]) {
		case PUT: s.buf[0].push_back({W, 1}); break;
		case FENCE: drain(s, 0); break;
		case LOAD_A: s.next = load(s, 0, A); break;
		case CAS_A:   // while (next > due && !cas(next, due)) {}
			if (s.next > kDue) {
				drain(s, 0);
				s.next = s.mem[A];
				if (s.next > kDue) {
					s.mem[A] = kDue;
				}
			}
			break;
		case LOAD_S: s.sleeping = load(s, 0, S); break;
	}
	if (s.pc[0] == static_cast<int>(prog.size())) {
		s.signalled = s.sleeping && (s.next >= kDue || s.next <= kNow);
	}
}

// Every interleaving of the two threads' steps and buffer drains; counts executions that end with
// the worker asleep past the task and no signal.
static long explore(State s, const std::vector<Op>& prog, long& runs) {
	bool p_done = s.pc[0] == static_cast<int>(prog.size()), w_done = s.pc[1] == kWorkerSteps;
	if (p_done && w_done) {
		drain(s, 0);
		drain(s, 1);
		++runs;
		return !s.signalled && s.n > kDue ? 1 : 0;
	}
	long lost = 0;
	if (!p_done) {
		State t = s;
		producer_step(t, prog);
		lost += explore(t, prog, runs);
	}
	if (!w_done) {
		State t = s;
		worker_step(t);
		lost += explore(t, prog, runs);
	}
	for (int th = 0; th < 2; ++th) {
		if (!s.buf[th].empty()) {
			State t = s;
			t.mem[t.buf[th].front().v] = t.buf[th].front().x;
			t.buf[th].erase(t.buf[th].begin());
			lost += explore(t, prog, runs);
		}
	}
	return lost;
}

// The producer side of `add_at(task, now)`, in source order.
static std::vector<Op> read_add(const char* path) {
	std::ifstream in(path);
	std::stringstream ss;
	ss << in.rdbuf();
	std::string src = ss.str();
	// From the signature to the load of `sleeping` that decides the signal (the trace calls after it
	// read the published time too, but only to print it).
	auto begin = src.find("schedule_result add_at(const std::shared_ptr<T>& task, std::chrono::steady_clock::time_point now) noexcept {");
	auto end = begin == std::string::npos ? begin : src.find("sleeping.load()", begin);
	if (end != std::string::npos) {
		end += std::string("sleeping.load()").size();
	}
	if (end == std::string::npos) {
		std::fprintf(stderr, "add_at(task, now) not found in %s\n", path);
		std::exit(1);
	}
	src = src.substr(begin, end - begin);
	struct Pattern { const char* text; Op op; };
	static const Pattern patterns[] = {
		{"scheduler_queue.add(", PUT},
		{"std::atomic_thread_fence(std::memory_order_seq_cst)", FENCE},
		{"atom_next_wakeup_time.load()", LOAD_A},
		{"atom_next_wakeup_time.compare_exchange", CAS_A},
		{"sleeping.load()", LOAD_S},
	};
	std::vector<std::pair<size_t, Op>> found;
	for (const auto& p : patterns) {
		for (auto at = src.find(p.text); at != std::string::npos; at = src.find(p.text, at + 1)) {
			found.push_back({at, p.op});
		}
	}
	std::sort(found.begin(), found.end());
	std::vector<Op> prog;
	for (auto& f : found) {
		prog.push_back(f.second);
	}
	return prog;
}

static long check(const std::vector<Op>& prog, const char* label) {
	std::printf("%s:", label);
	for (auto o : prog) {
		std::printf(" %s", op_name(o));
	}
	std::printf("\n");
	long lost_all = 0;
	for (int x : {5, 20, 50}) {
		stale = x;
		long runs = 0;
		long lost = explore(State{}, prog, runs);
		std::printf("  published %d (now %d, task %d): %ld of %ld executions leave the task waiting for the idle target\n", x, kNow, kDue, lost, runs);
		lost_all += lost;
	}
	return lost_all;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s scheduler.h\n", argv[0]);
		return 2;
	}
	auto prog = read_add(argv[1]);
	if (prog.size() != 5 || prog.front() != PUT || prog.back() != LOAD_S) {
		std::fprintf(stderr, "add() no longer has the expected five operations; update this model\n");
		return 1;
	}
	long lost = check(prog, "add() in scheduler.h");
	long old_lost = check({PUT, LOAD_A, CAS_A, FENCE, LOAD_S}, "add() with the fence after the loads (before the S1 fix)");
	if (old_lost == 0) {
		std::fprintf(stderr, "the model no longer finds the loss in the old order: it is not checking anything\n");
		return 1;
	}
	if (lost != 0) {
		std::fprintf(stderr, "add() can lose a wake-up: read the published time and `sleeping` after the fence\n");
		return 1;
	}
	std::printf("handshake model OK\n");
	return 0;
}
