class RoundRobinScheduler {
    int nextWorker = 1;

public:
    int selectWorker() {
        int worker = nextWorker;
        nextWorker = nextWorker % 3 + 1;
        return worker;
    }
};