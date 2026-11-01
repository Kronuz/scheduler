// Diagnostic driver around bench_post.cc, for A/B work on the add() and fire rows: one process times one
// mode for ROUNDS runs and prints the median, min and max, so a shell loop can interleave binaries built
// against different checkouts (A, B, A again) and take medians over processes.
//
//   diag_post MODE [ROUNDS]     MODE: make, add, add4, post, queue, queue4, fire, firequeue, size
//
// Build against a checkout SRC with the same flags as the bench (-O3 -DNDEBUG), including SRC and SRC/bench:
//   c++ -std=c++20 -O3 -DNDEBUG -I SRC -I SRC/bench -I stash -I threadpool -I random \
//       diag_post.cc threadpool/thread.cc random/random.cc -lpthread
#define main bench_main
#include "bench_post.cc"
#undef main

int main(int argc, char** argv) {
	std::string mode = argc > 1 ? argv[1] : "add";
	if (mode == "size") {
		std::printf("sizeof(ScheduledTask) %zu  sizeof(Trivial) %zu\n", sizeof(BenchBase), sizeof(Trivial));
		return 0;
	}
	int rounds = argc > 2 ? std::atoi(argv[2]) : 15;
	std::vector<double> s;
	for (int r = 0; r < rounds; ++r) {
		double v;
		if (mode == "make") v = run(Mode::make_only, 1, 200000);
		else if (mode == "add") v = run(Mode::add, 1, 200000);
		else if (mode == "add4") v = run(Mode::add, 4, 200000);
		else if (mode == "post") v = run(Mode::post, 1, 200000);
		else if (mode == "queue") v = run(Mode::queue, 1, 200000);
		else if (mode == "queue4") v = run(Mode::queue, 4, 200000);
		else if (mode == "fire") v = fire(true, 200000);
		else if (mode == "firequeue") v = fire(false, 200000);
		else return 2;
		s.push_back(v);
	}
	std::sort(s.begin(), s.end());
	std::printf("%-10s median %7.1f  min %7.1f  max %7.1f ns per task (%d runs)\n", mode.c_str(), s[s.size() / 2], s[0], s.back(), rounds);
	return 0;
}
