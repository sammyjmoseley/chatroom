#include <iostream>
#include <message.pb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <message.pb.h>

const bool debug = false;

struct Packet {
    int id;
    int client_socket;
};

struct MessageId {
    int from_id;
    int to_id;
    int lc_id;

    bool operator ==(const MessageId a) const {
        return from_id==a.from_id && to_id == a.to_id && lc_id == a.lc_id;
    }
};

size_t message_id_hash( const MessageId & name ) {
    return std::hash<int>()(name.from_id) ^ std::hash<int>()(name.to_id) ^ std::hash<int>()(name.lc_id);
}

template <class T>
class ConcurrentQueue {
    std::queue<T> queue;
    std::mutex mtx;
    std::condition_variable cv;

public:
    void push(T elem) {
        try {
            std::unique_lock<std::mutex> lck (mtx, std::defer_lock);
            queue.push(elem);
        }
        catch (std::logic_error&) {
            std::cout << "[exception caught]\n";
        }
        cv.notify_one();
    }

    T pop() {

        try {
            std::unique_lock<std::mutex> lck(mtx);
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

template <class L, class R>
class BiMap {
    std::map<L, R> left;
    std::map<R, L> right;

    void removeLeftHelper(L l) {
        typename std::map<L, R>::const_iterator itL = left.find(l);
        if (itL == left.end()) {
            return;
        }

        left.erase(itL);
    }

    void removeRightHelper(R r) {
        typename std::map<R, L>::const_iterator itR = right.find(r);
        if (itR == left.end()) {
            return;
        }
        right.erase(itR);
    }

public:
    void add(L l, R r) {
        left.insert(std::pair<L, R>(l, r));
        right.insert(std::pair<L, R>(r, l));
    }

    void removeLeft(L l) {
        typename std::map<L, R>::iterator itL = left.find(l);
        if (itL == left.end()) {
            return;
        }
        removeRightHelper(itL->second);
        left.erase(itL);
    }

    void removeRight(R r) {
        typename std::map<R, L>::iterator itR = right.find(r);
        if (itR == right.end()) {
            return;
        }
        removeLeftHelper(itR->second);
        right.erase(itR);
    }

    typename std::map<L, R>::iterator beginLeft() {
        return left.begin();
    };

    typename std::map<R, L>::iterator beginRight() {
        return right.begin();
    };

    typename std::map<L, R>::const_iterator endLeft() {
        return left.end();
    };

    typename std::map<R, L>::const_iterator endRight() {
        return right.end();
    };
};


struct NodeThread {
    int port_num;
    std::atomic<bool> is_alive;
    ConcurrentQueue<char*>* message_in_queue;
    ConcurrentQueue<Packet>* message_out_queue;
    std::unordered_set<MessageId, decltype(&message_id_hash)>* message_set;
    int len_clients_socks;
    int* client_sockets;

    void (*conn_accepter)(int newsockfd, char* buffer, NodeThread* node_thread);
};

void error(const char *msg) {
    perror(msg);
    exit(1);
}


void create_socket(NodeThread * node_thread) {
    int sockfd;
    int opt = true;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(node_thread->port_num);

    if( setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&(opt),
                   sizeof(opt)) < 0 ) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    if (bind(sockfd, (struct sockaddr *) &serv_addr,
             sizeof(serv_addr)) < 0) {
        error("ERROR on binding");
    }
    listen(sockfd,5);
    clilen = sizeof(cli_addr);

    fd_set readfds;


    for (int i = 0; i < node_thread->len_clients_socks; i++) {
        node_thread->client_sockets[i] = 0;
    }

    while (node_thread -> is_alive) {
        FD_ZERO(&readfds);

        FD_SET(sockfd, &readfds);
        int max_fd = sockfd;
        for (int i = 0; i < node_thread->len_clients_socks; i++) {
            int sd = node_thread->client_sockets[i];
            if (sd > 0) {
                FD_SET(sd, &readfds);
            }
            if (sd > max_fd) {
                max_fd = sd;
            }
        }

        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);

        if ((activity < 0) && errno != EINTR) {
            printf("select error");
        }

        if (FD_ISSET(sockfd, &readfds))
        {
            int new_socket;
            struct sockaddr_in address;
            int addrlen = sizeof(address);
            char* message = "ECHO Daemon v1.0 \r\n";
            if ((new_socket = accept(sockfd,
                                     (struct sockaddr *)&address, (socklen_t*)&addrlen))<0)
            {
                perror("accept");
                exit(EXIT_FAILURE);
            }

            //inform user of socket number - used in send and receive commands
            printf("New connection , socket fd is %d , ip is : %s , port : %d \n" , new_socket , inet_ntoa(address.sin_addr) , ntohs
                    (address.sin_port));

            //send new connection greeting message
//            if( send(new_socket, message, strlen(message), 0) != strlen(message) )
//            {
//                perror("send");
//            }

            puts("Welcome message sent successfully");

            //add new socket to array of sockets
            for (int i = 0; i < node_thread->len_clients_socks; i++)
            {
                //if position is empty
                if( node_thread->client_sockets[i] == 0 )
                {
                    node_thread->client_sockets[i] = new_socket;
                    printf("Adding to list of sockets as %d\n" , i);

                    break;
                }
            }
        }

        //else its some IO operation on some other socket
        for (int i = 0; i < node_thread->len_clients_socks; i++)
        {
            int sd = node_thread->client_sockets[i];

            if (FD_ISSET( sd , &readfds))
            {
                //Check if it was for closing , and also read the
                //incoming message
                int valread;
                char buffer[1024];
                sockaddr_in address;
                int addrlen = sizeof(addrlen);
                if ((valread = read( sd , buffer, 1024)) == 0)
                {
                    //Somebody disconnected , get his details and print
                    getpeername(sd , (struct sockaddr*)&address , \
                        (socklen_t*)&addrlen);
                    printf("Host disconnected , ip %s , port %d \n" ,
                           inet_ntoa(address.sin_addr) , ntohs(address.sin_port));

                    //Close the socket and mark as 0 in list for reuse
                    close( sd );
                    node_thread->client_sockets[i] = 0;
                }

                    //Echo back the message that came in
                else
                {
                    //set the string terminating NULL byte on the end
                    //of the data read
                    buffer[valread] = '\0';
                    node_thread->message_in_queue->push(buffer);
                }
            }
        }
    }
}

void message_reader(NodeThread * nodeThread) {
    while (nodeThread->is_alive.load()) {
        char* txt = nodeThread->message_in_queue->pop();
//        ChatMessage chatMessage;
//        if (!chatMessage.ParseFromString(txt)) {
//            std::cout << "error parsing string" << std::endl;
//        }
        MessageId messageId;
//        messageId.lc_id = chatMessage.lc_id();
//        messageId.from_id = chatMessage.from_id();
//        messageId.to_id = chatMessage.to_id();
        if (nodeThread->message_set->find(messageId) == nodeThread->message_set->end()) {
            std::cout << txt << std::endl;
            nodeThread->message_set->insert(messageId);
        } else {
            std::cout << "already recieved this message" << std::endl;
        }


    }
}

void message_writer(NodeThread nodeThread) {
    while(nodeThread.is_alive.load()) {

    }
}



int main(int argc, char * argv[]) {
    if (argc != 4) {
        error("Incorrect number of arguments, 3 required");
    }
    int id = atoi(argv[1]);
    int n = atoi(argv[2]);
    int port = atoi(argv[3]);


    const int max_num_sockets = 4;

    ConcurrentQueue<char*> message_in_queue;
    ConcurrentQueue<Packet> message_out_queue;
    std::unordered_set<MessageId, decltype(&message_id_hash)> message_set(100, message_id_hash);
    int client_socket[max_num_sockets];

    NodeThread nodeThread;
//    nodeThread.address = "localhost";
    nodeThread.message_in_queue = &message_in_queue;
    nodeThread.message_out_queue = &message_out_queue;
    nodeThread.message_set = &message_set;
    nodeThread.is_alive.store(true);
    nodeThread.client_sockets = client_socket;
    nodeThread.len_clients_socks = max_num_sockets;
    nodeThread.port_num = port;


    std::thread connection_thread(create_socket, &nodeThread);

    std::thread reading_thread(message_reader, &nodeThread);

    reading_thread.join();

    std::cout << std::endl;

    exit(0);
}