
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <vector>


/*
 * Manage operational metrics
 */
namespace metrics {

    using namespace std;

    // could move into registry class and trait-ize I guess
    using clock = chrono::system_clock;
    using period = chrono::nanoseconds;
    using time_point = chrono::time_point<clock>;
    using dur_at_time = pair<time_point, period>;
    using count_type = unsigned long;
    using count_at_time = pair<time_point, count_type>;
    using gauged_type = double;
    using gauge_at_time = pair<time_point, gauged_type>;

    // Passkey so only metrics registry / metrics internals can construct tokens
    class Internal {
        friend class registry;

    private:
        explicit Internal() = default;
    };


    /*
     * "Tokens" do the heavy lifting. They track
     * values at points in time. A token is
     * intended to live as long as the metrics
     * registry that creates it.
     *
     * They are intended to be thread-safe!
     *
     * They are bound to a string name for
     * reporting purposes within the metrics
     * registry, but the tokens themselves don't
     * know these names.
     *
     * For now they pre-allocate heap-space for
     * 10e6 data-points (and don't yet barf if
     * they auto-expand past that point). If an
     * application wishes to report more than this,
     * we need to consider some "flush" operations
     * or something.
     */
    namespace token {

        /*
         * Represents a timing at a point in time.
         * E.g. "at noon the query took 10ms".
         */
        class time_token {
            // allocate for 10 million data-points
            vector<dur_at_time> durations{10 * 1000 * 1000};

        public:
            time_token(time_token &&other) noexcept
                    : durations{move(other.durations)} {}

            explicit time_token(Internal) {}

            // Don't care about ordering just need to be
            // able to store in unordered_map.
            bool operator<(const time_token &rhs) {
                return addressof(*this) < addressof(rhs);
            }

            time_token(const time_token &) = delete;

            void operator=(const time_token &) = delete;

            /**
             * Example usage:
             *   auto x = token(); // starts clock
             *      // do something useful
             *   x.stop(); // stops clock and reports timing
             */
            class stopper;

            stopper operator()() {
                return {clock::now(), *this};
            }

            class stopper {
                const time_point start;
                time_token &time_tok;
            public:
                stopper(const stopper &) = delete;

                void operator=(const stopper &) = delete;

                stopper(time_point start, time_token &time_tok)
                        : start{start}, time_tok{time_tok} {}

                stopper(stopper &&other) noexcept
                        : start{other.start}, time_tok{other.time_tok} {}

                // Technically you could call .stop() multiple times
                // and report data multiple times. Nothing "incorrect"
                // about this.
                void stop() {
                    const chrono::time_point<clock> now{clock::now()};
                    time_tok.durations.emplace_back(
                            now,
                            chrono::duration_cast<period>(now - start)
                    );
                }
            };
        };

        /*
         * A handle to a count value that is theoretically unknown by
         * a component of an application: components just know that they
         * affected the count in some way. E.g. "at 2pm I handled 3 requests"
         */
        class count_token {
            vector<count_at_time> counts{10 * 1000 * 1000};
            atomic<count_type> current{0};
        public:

            explicit count_token(Internal) {}

            count_token(const count_token &) = delete;

            void operator=(const count_token &) = delete;

            bool operator<(const count_token &rhs) {
                return addressof(rhs) < addressof(*this);
            }

            void operator++(int) {
                operator+=(1);
            }

            count_token &operator+=(count_type many) {
                counts.emplace_back(clock::now(), current += many);
                return *this;
            }
        };

        /*
         * Indicates a current observed value. E.g. "at 1pm there were 7 threads"
         */
        class gauge_token {
            vector<gauge_at_time> readings{10 * 1000 * 1000};;
        public:
            explicit gauge_token(Internal) {};

            gauge_token(const gauge_token &) = delete;

            void operator=(const gauge_token &other) = delete;

            gauge_token(gauge_token &&other) noexcept
                    : readings{move(other.readings)} {}

            bool operator<(const gauge_token &rhs) {
                return addressof(rhs) < addressof(*this);
            }

            void set(gauged_type val) {
                readings.emplace_back(clock::now(), val);
            }

            // A bit cutesy: lets us say e.g. `gauge = 6`.
            gauge_token &operator=(gauged_type val) {
                set(val);
                return *this;
            }

            explicit operator gauged_type() {
                return readings.empty()
                       ? static_cast<gauged_type>(0)
                       : readings.back().second;
            }
        };
    }

    /*
     * Exposes access to tokens.
     *
     * An application is intended to register
     * its tokens only during startup.
     *
     * Eventually this will have methods to actually
     * report timings / statistics.
     */
    class registry {

        template<typename T>
        using entries = vector<pair<string, T>>;

        unordered_map<string, token::time_token> timers = {};
        unordered_map<string, token::count_token> counters = {};
        unordered_map<string, token::gauge_token> gauges = {};

    public:

        static registry create() {
            return registry(Internal{});
        }

        explicit registry(Internal) {}

        registry(registry &) = delete;

        void operator=(const registry &) = delete;

        registry(registry &&other) noexcept
                : timers{move(other.timers)},
                  counters{move(other.counters)},
                  gauges{move(other.gauges)} {}

        // It's a bit awkward to return &
        // but not sure of better way...

        token::time_token &run(const string &name) {
            timers.emplace(name, Internal{});
            return timers.at(name);
        }

        token::count_token &count(const string &name) {
            counters.emplace(name, Internal{});
            return counters.at(name);
        }

        token::gauge_token &gauge(const string &name) {
            gauges.emplace(name, Internal{});
            return gauges.at(name);
        }
    };

    // RAII-style version of stopper.
    // Starts clock when constructed. Stops and reports when goes out of scope.
    class timed {
        token::time_token::stopper stopper;
    public:
        explicit timed(token::time_token &time_tok)
                : stopper{time_tok()} {}

        ~timed() {
            stopper.stop();
        }
    };
}

// Example usage
int main() {
    // Intended to have one of these for app lifecycle
    metrics::registry m{metrics::registry::create()};

    // Register tokens at application startup.
    // (bad-style to require & here?)
    auto &query_tok{m.run("query")};
    auto &gauge_tok{m.gauge("threads")};
    auto &count_t{m.count("completed")};


    auto oper = query_tok();
    std::cout << "Hello, world!" << std::endl;
    gauge_tok.set(100);
    gauge_tok = 50;
    count_t++;
    count_t += 100;
    // cute but not really necessary:
    std::cout << "Casting gauge to double: " << double(gauge_tok) << std::endl;
    oper.stop();

    {
        std::cout << "Starting block" << std::endl;
        metrics::timed t{query_tok};
        std::cout << "Ending block" << std::endl;
    }

    return EXIT_SUCCESS;
}
