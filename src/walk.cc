// Algorithm W from Knuth's The Art of Computer Programming 7.2.2.2: WalkSAT.

// This program either finds a satisfying assignment or runs forever.

#include <sstream>
#include <vector>

#include "counters.h"
#include "flags.h"
#include "logging.h"
#include "timer.h"
#include "types.h"

extern unsigned long FLAGS_seed;

DEFINE_PARAM(initial_bias, 0.1,
             "Probability that true is selected for each variable during "
             "initial random assignment.");

DEFINE_PARAM(non_greedy_choice, 0.65,
             "Probability that we will choose a flip literal from all literals "
             "in a clause instead of from all minimum cost literals.");

// Flips a coin that lands on heads with probability p. Return true iff heads.
static bool flip(float p) {
    return static_cast<float>(rand())/RAND_MAX <= p;
}

struct Cnf {
    // Clauses are stored as a sequential list of literals in memory with no
    // terminator between clauses. Example: (1 OR 2) AND (3 OR -2 OR -1) would
    // be stored as [1][2][3][-2][-1]. The start array (below) keeps track of
    // where each clause starts -- in the example above, start[0] = 0 and
    // start[1] = 2. The end index of each clause can be inferred from the start
    // index of the next clause.
    std::vector<lit_t> clauses;

    // Zero-indexed map of clauses. Literals in clause i run from
    // clauses[start[i]] to clauses[start[i+1]] - 1 except for the final
    // clause, where the endpoint is just clauses.size() - 1. start.size() is
    // the number of clauses.
    std::vector<clause_t> start;

    // One-indexed values of variables in the satisfying assignment.
    std::vector<bool> val;

    // One-indexed costs of variables.
    std::vector<clause_t> cost;

    // Maps literals -> list of clauses the literal is in.
    std::vector<std::vector<clause_t>> invclause_storage;
    std::vector<clause_t>* invclause;

    // Stack of unsatisfied clauses.
    std::vector<lit_t> f;

    // Number of true literals in clause
    std::vector<lit_t> numtrue;

    // Reverse lookup into unsatisfied clauses. if f[i] = j, w[j] = i.
    std::vector<clause_t> w;

    // Number of variables in the formula. Valid variables range from 1 to
    // nvars, inclusive.
    lit_t nvars;

    clause_t nclauses;

    Cnf(lit_t nvars, clause_t nclauses) :
        val(nvars+1),
        cost(nvars+1, 0),
        invclause_storage(2 * nvars + 1),
        invclause(&invclause_storage[nvars]),
        numtrue(nclauses, 0),
        w(nclauses, clause_nil),
        nvars(nvars),
        nclauses(nclauses) {
        if (FLAGS_seed == 0) FLAGS_seed = time(NULL);
        srand(FLAGS_seed);
    }

    // These two methods give the begin/end index of the kth clause in the
    // clauses vector. Used for iterating over all literals in the kth clause.
    inline clause_t clause_begin(clause_t c) const { return start[c]; }
    inline clause_t clause_end(clause_t c) const {
        return (c == start.size() - 1) ? clauses.size() : start[c + 1];
    }

    inline bool is_true(lit_t l) {
        bool tv = val[var(l)];
        return (tv && l > 0) || (!tv && l < 0);
    }

    bool is_satisfied(clause_t c) {
        clause_t end = clause_end(c);
        for (clause_t itr = clause_begin(c); itr < end; ++itr) {
            if (is_true(clauses[itr])) return true;
        }
        return false;
    }

    void register_satisfied(clause_t c) {
        if (w[c] == clause_nil) return;
        w[f[f.size()-1]] = w[c];
        std::swap(f[w[c]], f[f.size()-1]);
        w[c] = clause_nil;
        f.resize(f.size()-1);
    }

    void register_unsatisfied(clause_t c) {
        if (w[c] != clause_nil) return;
        w[c] = f.size();
        f.push_back(c);
    }

    std::string dump_raw() {
        std::ostringstream oss;
        for (lit_t c : clauses) {
            oss << c << " ";
        }
        return oss.str();
    }

    std::string dump_clause(clause_t c) {
        std::ostringstream oss;
        clause_t end = clause_end(c);
        oss << "(";
        for (clause_t itr = clause_begin(c); itr < end; ++itr) {
            oss << clauses[itr];
            if (is_true(clauses[itr])) oss << "*";
            if (itr < end - 1) oss << " ";
        }
        oss << ")";
        return oss.str();
    }

    std::string dump_clauses() {
        std::ostringstream oss;
        for (size_t i = 0; i < start.size(); ++i) {
            oss << dump_clause(i);
        }
        return oss.str();
    }

    std::string dump_unsat() {
        std::ostringstream oss;
        for (clause_t fi : f) {
            oss << "[" << fi << "] " << dump_clause(fi) << ", ";
        }
        return oss.str();
    }

    void print_assignment() {
        for (int i = 1, j = 0; i <= nvars; ++i) {
            if (j % 10 == 0) PRINT << "v";
            PRINT << (val[i] ? " -" : " ") << i;
            ++j;
            if (i == nvars) PRINT << " 0" << std::endl;
            else if (j > 0 && j % 10 == 0) PRINT << std::endl;
        }
    }
};

// Parse a DIMACS cnf input file. File starts with zero or more comments
// followed by a line declaring the number of variables and clauses in the file.
// Each subsequent line is the zero-terminated definition of a disjunction.
// Clauses are specified by integers representing literals, starting at 1.
// Negated literals are represented with a leading minus.
//
// Example: The following CNF formula:
//
//   (x_1 OR x_2) AND (x_3) AND (NOT x_2 OR NOT x_3 OR x_4)
//
// Can be represented with the following file:
//
// c Header comment
// p cnf 4 3
// 1 2 0
// 3 0
// -2 -3 4 0
Cnf parse(const char* filename) {
    int nc;
    FILE* f = fopen(filename, "r");
    CHECK(f) << "Failed to open file: " << filename;

    // Read comment lines until we see the problem line.
    long long nvars = 0, nclauses = 0;
    do {
        nc = fscanf(f, " p cnf %lld %lld \n", &nvars, &nclauses);
        if (nc > 0 && nc != EOF) break;
        nc = fscanf(f, "%*s\n");
    } while (nc != 2 && nc != EOF);
    CHECK(nvars >= 0);
    CHECK(nclauses >= 0);
    CHECK_NO_OVERFLOW(lit_t, nvars);
    CHECK_NO_OVERFLOW(clause_t, nclauses);

    Cnf c(static_cast<lit_t>(nvars), static_cast<clause_t>(nclauses));

    // Read clauses until EOF.
    int lit;
    do {
        bool read_lit = false;
        std::size_t start = c.clauses.size();
        while (true) {
            nc = fscanf(f, " %i ", &lit);
            if (nc == EOF || lit == 0) break;
            c.clauses.push_back(lit);
            read_lit = true;
        }
        if (nc != EOF && start == c.clauses.size()) {
            LOG(2) << "Empty clause in input file, unsatisfiable formula.";
            UNSAT_EXIT;
        }
        if (!read_lit) break;
        c.start.push_back(start);
    } while (nc != EOF);
    CHECK(c.start.size() == c.nclauses) << "Expected nclauses == start.size()";

    fclose(f);
    return c;
}

// Returns true exactly when a satisfying assignment exists for c.
// TODO: call solve repeatedly with reluctant doubling sequence.
bool solve(Cnf* c) {
    Timer t("solve");

    // W1. [Initialize.]
    for (lit_t i = 1; i <= c->nvars; ++i) {
        c->val[i] = flip(PARAM_initial_bias);
    }
    for (clause_t i = 0; i < c->nclauses; ++i) {
        clause_t end = c->clause_end(i);
        lit_t tl = lit_nil;
        for (clause_t j = c->clause_begin(i); j < end; ++j) {
            // Note: if a literal appears twice in a clause, the clause index
            // will appear twice in invclause.
            c->invclause[c->clauses[j]].push_back(i);
            if (c->is_true(c->clauses[j])) {
                ++c->numtrue[i];
                tl = var(c->clauses[j]);
            }
        }
        if (c->numtrue[i] == 0) {
            c->w[i] = c->f.size();
            c->f.push_back(i);
        } else if (c->numtrue[i] == 1) {
            ++c->cost[tl];
        }
    }

    while (true) {
        LOG(3) << c->dump_raw();
        LOG(2) << c->dump_clauses();

        // W2. [Done?]
        if (c->f.empty()) return true;
        // TODO: terminate with UNKNOWN if num iterations is too large?

        // W3. [Choose j.]
        LOG(3) << "Unsat clauses: " << c->dump_unsat();
        CHECK_NO_OVERFLOW(clause_t, RAND_MAX);
        clause_t divisor = (static_cast<clause_t>(RAND_MAX) + 1)/c->f.size();
        clause_t q;
        do { q = std::rand() / divisor; } while (q >= c->f.size());
        LOG(2) << "Chose clause " << q << ": " << c->dump_clause(c->f[q]);

        // W4. [Choose l.]
        bool all = flip(PARAM_non_greedy_choice);
        clause_t end = c->clause_end(c->f[q]);
        lit_t choice = lit_nil;
        int k = 1;
        lit_t min_cost = std::numeric_limits<lit_t>::max();
        for (clause_t itr = c->clause_begin(c->f[q]); itr < end; ++itr) {
            lit_t cost = c->cost[var(c->clauses[itr])];
            LOG(3) << var(c->clauses[itr]) << " has cost " << cost;
            if (cost < min_cost) {
                min_cost = cost;
                if (!all || min_cost == 0) k = 1;
            }
            if ((all && min_cost > 0) || cost == min_cost) {
                if (flip(1.0/k)) { choice = c->clauses[itr]; }
                ++k;
            }
        }
        CHECK(choice != lit_nil) << "No flip literal chosen.";

        LOG(2) << "Chose " << choice << " to flip. (cost = "
               << c->cost[var(choice)] << ")";

        // W5. [Flip l.]
        lit_t pos = (c->val[var(choice)] == (choice > 0)) ? choice : -choice;
        lit_t neg = -pos;

        c->val[var(choice)] = !c->val[var(choice)];

        // TODO: during updates below, move literals to front of clause if they
        // might hit the second loop (some other variable ...) later.

        // Iterate over all clauses where choice was true but is now false.
        for (clause_t i : c->invclause[pos]) {
            --c->numtrue[i];
            if (c->numtrue[i] == 0) {
                // Clause is newly unsatisfied.
                c->register_unsatisfied(i);
                --c->cost[var(choice)];
            } else if (c->numtrue[i] == 1) {
                // Some other variable in the clause needs its cost incremented.
                clause_t end = c->clause_end(i);
                for (clause_t itr = c->clause_begin(i); itr < end; ++itr) {
                    if (c->is_true(c->clauses[itr])) {
                        ++c->cost[var(c->clauses[itr])];
                        break;
                    }
                }
            }
        }

        // Iterate over all clauses where choice was false but is now true.
        for (clause_t i : c->invclause[neg]) {
            ++c->numtrue[i];
            if (c->numtrue[i] == 1) {
                // Clause is newly satisfied.
                c->register_satisfied(i);
                ++c->cost[var(choice)];
            } else if (c->numtrue[i] == 2) {
                // Some other variable in the clause needs its cost decremented.
                clause_t end = c->clause_end(i);
                for (clause_t itr = c->clause_begin(i); itr < end; ++itr) {
                    if (c->clauses[itr] != neg && c->is_true(c->clauses[itr])) {
                        --c->cost[var(c->clauses[itr])];
                        break;
                    }
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    int oidx;
    CHECK(parse_flags(argc, argv, &oidx))
        << "Usage: " << argv[0] << " <filename>";
    init_counters();
    init_timers();
    Cnf c = parse(argv[oidx]);
    if (c.clauses.empty() || solve(&c)) {
        SAT_EXIT(&c);
    }
}
