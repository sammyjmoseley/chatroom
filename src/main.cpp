#include <iostream>
#include <message.pb.h>
#include <unordered_set>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

typedef std::unordered_set<int> int_set;

template <class T>
class ConcurrentQueue {
    std::queue<T> queue;
    std::mutex mtx;
    std::condition_variable cv;

public:
    void push(T elem) {
        try {
            std::unique_lock<std::mutex> lck (mtx);
            queue.push(elem);
        }
        catch (std::logic_error&) {
            std::cout << "[exception caught]\n";
        }
        cv.notify_one();
    }

    T pop() {
        try {
            std::unique_lock<std::mutex> lck (mtx);
            this->cv.wait(lck, [this]{return !queue.empty();});
            char* ret = queue.front();
            queue.pop();
            return ret;
        }
        catch (std::logic_error&) {
            std::cout << "[exception caught]\n";
        }
    }
};

struct NodeThread {
    int portno;
    std::atomic<bool> is_alive;
    ConcurrentQueue<char*> concurrentQueue;
};




void error(const char *msg) {
    perror(msg);
    exit(1);
}


void update_node(int_set* set, int id, bool is_alive) {
    int_set::const_iterator it = set->find(id);
    bool contains = !(it == set->end());
    if (is_alive && !contains) {
        set->insert(id);
    } else if (!is_alive && contains) {
        set->erase(id);
    }
}

void create_socket(NodeThread * node_thread) {
    int sockfd, newsockfd;
    socklen_t clilen;
    char buffer[256];
    struct sockaddr_in serv_addr, cli_addr;
    int n;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(node_thread->portno);
    if (bind(sockfd, (struct sockaddr *) &serv_addr,
             sizeof(serv_addr)) < 0) {
        error("ERROR on binding");
    }
    listen(sockfd,5);
    clilen = sizeof(cli_addr);


    while (node_thread->is_alive.load()) {
        newsockfd = accept(sockfd,
                           (struct sockaddr *) &cli_addr,
                           &clilen);
        if (newsockfd < 0)
            error("ERROR on accept");
        while (node_thread->is_alive.load()) {
            bzero(buffer, 256);
            n = read(newsockfd, buffer, 255);
            if (n <= 0) {
                break;
            }

            printf("Here is the message: %s\n", buffer);

            char buffer_copy[256];
            memcpy(&buffer_copy, &buffer, sizeof(buffer));
            node_thread->concurrentQueue.push(buffer_copy);
        }
    }
    std::cout << "closing socket";
    close(newsockfd);
    close(sockfd);
}

int main(int argc, char * argv[]) {
    if (argc != 4) {
        error("Incorrect number of arguments, 3 required");
    }
    int id = atoi(argv[1]);
    int n = atoi(argv[2]);
    int port = atoi(argv[3]);

    NodeThread nodeThread;
    nodeThread.is_alive.store(true);
    nodeThread.portno = port;

    printf("start %i id on port %i with %i processes\n", id, port, n);
    ChatMessage* chatMessage;
    std::thread connection_thread(create_socket, &nodeThread);

    while (nodeThread.is_alive.load()) {
        char* txt = nodeThread.concurrentQueue.pop();
        printf("%s\n",txt);
    }

    exit(0);
}