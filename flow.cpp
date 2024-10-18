#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>

using namespace std;

class Node {
public:
    string name;
    string command;
};

class Pipe {
public:
    string name;
    string from;
    string to;
};

class Concatenate {
public:
    string name;
    vector<string> part_names;
};

map<string, Node> nodes;
map<string, Pipe> pipes;
map<string, Concatenate> concatenates;

void trim(string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == string::npos || end == string::npos)
        s = "";
    else
        s = s.substr(start, end - start + 1);
}

void parse_flow_file(const string &filename) {
    ifstream infile(filename.c_str());
    if (!infile.is_open()) {
        perror("Error opening .flow file");
        exit(EXIT_FAILURE);
    }

    string line;
    string current_section = "";
    string current_name = "";
    while (getline(infile, line)) {
        trim(line);
        if (line.empty())
            continue;

        size_t equal_pos = line.find('=');
        if (equal_pos == string::npos)
            continue;

        string key = line.substr(0, equal_pos);
        string value = line.substr(equal_pos + 1);
        trim(key);
        trim(value);

        if (key == "node") {
            current_section = "node";
            current_name = value;
            Node node;
            node.name = current_name;
            node.command = "";
            nodes[current_name] = node;
        } else if (key == "command" && current_section == "node") {
            nodes[current_name].command = value;
            current_section = "";
            current_name = "";
        } else if (key == "pipe") {
            current_section = "pipe";
            current_name = value;
            Pipe pipe_obj;
            pipe_obj.name = current_name;
            pipe_obj.from = "";
            pipe_obj.to = "";
            pipes[current_name] = pipe_obj;
        } else if (key == "from" && current_section == "pipe") {
            pipes[current_name].from = value;
        } else if (key == "to" && current_section == "pipe") {
            pipes[current_name].to = value;
            current_section = "";
            current_name = "";
        } else if (key == "concatenate") {
            current_section = "concatenate";
            current_name = value;
            Concatenate concat;
            concat.name = current_name;
            concatenates[current_name] = concat;
        } else if (key == "parts" && current_section == "concatenate") {
            // Do nothing, we will collect parts below
        } else if (key.substr(0, 5) == "part_" && current_section == "concatenate") {
            concatenates[current_name].part_names.push_back(value);
        } else {
            // Unknown key or outside of a section
        }
    }
    infile.close();
}

void executeItem(const string& itemName, int input_fd, int output_fd);

void executeNode(const Node& node, int input_fd, int output_fd) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        if (input_fd != STDIN_FILENO) {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }
        if (output_fd != STDOUT_FILENO) {
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }
        execl("/bin/sh", "sh", "-c", node.command.c_str(), (char *)NULL);
        perror("execl failed");
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        // Parent process
        if (input_fd != STDIN_FILENO) {
            close(input_fd);
        }
        if (output_fd != STDOUT_FILENO) {
            close(output_fd);
        }
        waitpid(pid, NULL, 0);
    } else {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }
}

void executePipe(const Pipe& pipe_obj, int input_fd, int output_fd) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe failed");
        exit(EXIT_FAILURE);
    }

    pid_t pid1 = fork();
    if (pid1 == 0) {
        // First child process ('from' item)
        close(pipefd[0]); // Close unused read end
        if (input_fd != STDIN_FILENO) {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        executeItem(pipe_obj.from, STDIN_FILENO, STDOUT_FILENO);
        exit(EXIT_SUCCESS);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        // Second child process ('to' item)
        close(pipefd[1]); // Close unused write end
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        if (output_fd != STDOUT_FILENO) {
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }
        executeItem(pipe_obj.to, STDIN_FILENO, STDOUT_FILENO);
        exit(EXIT_SUCCESS);
    }

    // Parent process
    close(pipefd[0]);
    close(pipefd[1]);
    if (input_fd != STDIN_FILENO) {
        close(input_fd);
    }
    if (output_fd != STDOUT_FILENO) {
        close(output_fd);
    }
    waitpid(pid1, NULL, 0);
    waitpid(pid2, NULL, 0);
}

void executeConcatenate(const Concatenate& concat, int input_fd, int output_fd) {
    for (const auto& partName : concat.part_names) {
        executeItem(partName, input_fd, output_fd);
    }
}

void executeItem(const string& itemName, int input_fd, int output_fd) {
    if (nodes.find(itemName) != nodes.end()) {
        executeNode(nodes[itemName], input_fd, output_fd);
    } else if (pipes.find(itemName) != pipes.end()) {
        executePipe(pipes[itemName], input_fd, output_fd);
    } else if (concatenates.find(itemName) != concatenates.end()) {
        executeConcatenate(concatenates[itemName], input_fd, output_fd);
    } else {
        cerr << "Error: Item '" << itemName << "' not found" << endl;
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        cerr << "Usage: ./flow <filename.flow> <action>" << endl;
        exit(EXIT_FAILURE);
    }

    const string flow_filename = argv[1];
    const string action_name = argv[2];

    parse_flow_file(flow_filename);

    if (nodes.find(action_name) != nodes.end() ||
        pipes.find(action_name) != pipes.end() ||
        concatenates.find(action_name) != concatenates.end()) {
        executeItem(action_name, STDIN_FILENO, STDOUT_FILENO);
    } else {
        cerr << "Error: Action '" << action_name << "' not found" << endl;
        exit(EXIT_FAILURE);
    }

    return 0;
}
