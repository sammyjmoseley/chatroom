#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <stdio.h>
#include <netdb.h>
#include <set>
#include <string>
const bool debug = true;
int NULL_INT = 0;

struct Packet {
    int id;
    int client_socket;
};

struct Message {
    int sockfd;
    char* message;
    int message_len;
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
            T ret = queue.front();
            queue.pop();
            return ret;
        }
        catch (std::logic_error&) {
            std::cout << "[exception caught]\n";
        }
    }
};

template <class K, class V>
class ConcurrentMap {

public:
    std::map<K, V> map;
    bool contains(K key) {
        return map.find(key) != map.end();
    }

    typename std::map<K, V>::iterator get(K key) {
        typename std::map<K,V>::iterator it = map.find(key);
        return it;
    };

    void put(K key, V val) {
        map.insert({{key, val}});
    }

    void remove(K key) {
        map.erase(key);
    }

    typename std::map<K, V> get_map() {
        return map;
    }

    typename std::map<K, V>::iterator it() {

        return map.begin();
    }

    typename std::map<K, V>::iterator end() {
        return map.end();
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
        std::map<int, int>::iterator lit = left.find(l);
        if (!(lit == left.end())) {
            removeRightHelper(lit->second);
            removeLeftHelper(lit->first);
        }

        std::map<int, int>::iterator rit = right.find(r);
        if (!(rit == right.end())) {
            removeLeftHelper(rit->second);
            removeRightHelper(rit->first);
        }


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

    typename std::map<L, R>::iterator endLeft() {
        return left.end();
    };

    typename std::map<R, L>::iterator endRight() {
        return right.end();
    };
};

template<class T>
class ConcurrentLinkedList {
    T val;
    ConcurrentLinkedList* next;
    ConcurrentLinkedList* prev;

public:
    ConcurrentLinkedList* add(T val) {
        ConcurrentLinkedList new_node;
        new_node.val = val;
        new_node.next = this->next;
        new_node.prev = this;
        this->next->prev = &new_node;
        return &new_node;
    }

    void remove() {
        if (this->prev != NULL) {
            this->prev->next = this->next;
        }

        if (this->next != NULL) {
            this->next->prev = this->prev;
        }
    }

    T get_val() {
        return val;
    }
};


struct NodeThread {
    int port_num;
    int my_id;
    std::atomic<bool> is_alive;
    ConcurrentQueue<Message*>* message_in_queue;
    ConcurrentQueue<Packet>* message_out_queue;
    std::unordered_set<MessageId, decltype(&message_id_hash)>* message_set;
    ConcurrentMap<int, const int*>* process_map; // process_id -> socket_fd
    ConcurrentMap<int, const int*>* socket_map; //socket_fd -> process_id

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

    while (node_thread -> is_alive) {
        FD_ZERO(&readfds);

        FD_SET(sockfd, &readfds);
        int max_fd = sockfd;
        for (std::map<int, const int*>::iterator it = node_thread->socket_map->it();
             it != node_thread->socket_map->end();
             ++it) {
            int sd = it->first;
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
            node_thread->socket_map->put(new_socket, &NULL_INT);


        }

        //else its some IO operation on some other socket
        std::set<int> removeBucket;
        for (auto it = node_thread->socket_map->it();
             it != node_thread->socket_map->end();
                ) {
            int sd = it->first;
            bool removed = false;
            if (FD_ISSET( sd , &readfds)) {
                //Check if it was for closing , and also read the
                //incoming message
                int valread;
                char buffer[1024];
                sockaddr_in address;
                int addrlen = sizeof(addrlen);



                if ((valread = read( sd , buffer, 1024)) == 0) {
                    //Somebody disconnected , get his details and print
                    getpeername(sd , (struct sockaddr*)&address , \
                        (socklen_t*)&addrlen);
                    printf("Host disconnected , ip %s , port %d \n" ,
                           inet_ntoa(address.sin_addr) , ntohs(address.sin_port));

                    //Close the socket and mark as 0 in list for reuse
                    close( sd );
                    if (node_thread->process_map->contains(*it->second)) {
                        node_thread->process_map->remove(*it->second);
                    }
                    removeBucket.insert(it->first);
                }

                    //Echo back the message that came in
                else {
                    //set the string terminating NULL byte on the end
                    //of the data read
                    buffer[valread] = '\0';
                    Message message;
                    message.message = buffer;
                    message.message_len = valread;
                    message.sockfd = sd;
                    node_thread->message_in_queue->push(&message);
                }
            }

            ++it;
        }

        for (auto it = removeBucket.begin(); it != removeBucket.end(); it++) {
            node_thread->socket_map->remove(*it);
        }
    }
}

struct heartbeat {
    int sockfd;
    int process_id;
};

struct Command {
    std::string* broadcast;
    hostent* connect;
    heartbeat* beat;
};

const std::string BROADCAST("broadcast ");
const std::string HEARTBEAT("heartbeat ");
const std::string DEADSIGNL("deadsignl ");
const std::string ALIVE("alive");

std::string* get_string(std::string* msg, const std::string* header) {
    if (msg->length() < header->length()) {
        return NULL;
    }
    long pos;
    if ((pos = msg->find(*header)) == -1) {
        return NULL;
    }
    std::string ret = msg->substr(pos+header->length());
    return &ret;
}

void parse_broadcast(Command* command, std::string* msg) {
    std::string* broadcast = get_string(msg, &BROADCAST);
    command->broadcast = broadcast;
}

void parse_heartbeat(NodeThread* nodeThread, int sockfd, std::string* msg) {
    std::string* str_id = get_string(msg, &HEARTBEAT);
    if (str_id == NULL) {
        return;
    }
    int id = atoi(&str_id->front());
    if (debug) {
        std::cout << "got id for " << id << std::endl;
    }
    if (!nodeThread->socket_map->contains(sockfd)) {
        nodeThread->socket_map->put(sockfd, NULL);
    }

    if (!nodeThread->process_map->contains(id)) {
        nodeThread->process_map->put(id, NULL);
    }

    nodeThread->socket_map->get(sockfd)->second = &nodeThread->process_map->get(id)->first;
    nodeThread->process_map->get(sockfd)->second = &nodeThread->socket_map->get(id)->first;
}

void parse_alive(NodeThread* nodeThread, std::string* msg) {
    std::string* str_alive = get_string(msg, &ALIVE);
//    if (str_alive == NULL) {
//        return;
//    }
    std::string str("alive ");
    for (auto it = nodeThread->process_map->it(); it != nodeThread->process_map->end(); ++it) {
        str.append(std::to_string(it->first));
        str.append(",");
    }

    std::cout << str.substr(0,str.length()-1) << std::endl;
}

Command* command_parse_message(std::string* msg) {
    Command command;
    parse_broadcast(&command, msg);
    return &command;
}

void message_reader(NodeThread * nodeThread) {
    while (nodeThread->is_alive.load()) {
        Message* txt = nodeThread->message_in_queue->pop();
        std::string msg(txt->message);
        Command* command = command_parse_message(&msg);
        if (command->broadcast != NULL) {
            std::cout << *command->broadcast<< std::endl;
        }

        parse_heartbeat(nodeThread, txt->sockfd, &msg);
        parse_alive(nodeThread, &msg);
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

    ConcurrentQueue<Message*> message_in_queue;
    ConcurrentQueue<Packet> message_out_queue;
    std::unordered_set<MessageId, decltype(&message_id_hash)> message_set(100, message_id_hash);
    int client_socket[max_num_sockets];
    ConcurrentMap<int, const int*> socket_map;
    ConcurrentMap<int, const int*> process_map;

    NodeThread nodeThread;
//    nodeThread.address = "localhost";
    nodeThread.my_id = id;
    nodeThread.message_in_queue = &message_in_queue;
    nodeThread.message_out_queue = &message_out_queue;
    nodeThread.message_set = &message_set;
    nodeThread.is_alive.store(true);
    nodeThread.socket_map = &socket_map;
    nodeThread.process_map = &process_map;
    nodeThread.port_num = port;


    std::thread connection_thread(create_socket, &nodeThread);

    std::thread reading_thread(message_reader, &nodeThread);

    reading_thread.join();

    std::cout << std::endl;

    exit(0);
}