#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
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

class StderrNode {
public:
    string name;
    string from;
};

class FileNode {
public:
    string name;
    string filename;
};

map<string, Node> nodes;
map<string, Pipe> pipes;
map<string, Concatenate> concatenates;
map<string, StderrNode> stderrNodes;
map<string, FileNode> fileNodes;

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

        // Specific conditions first
        if (current_section == "node" && key == "command") {
            nodes[current_name].command = value;
            current_section = "";
            current_name = "";
        } else if (current_section == "pipe" && key == "from") {
            pipes[current_name].from = value;
        } else if (current_section == "pipe" && key == "to") {
            pipes[current_name].to = value;
            current_section = "";
            current_name = "";
        } else if (current_section == "concatenate" && key.substr(0, 5) == "part_") {
            concatenates[current_name].part_names.push_back(value);
        } else if (current_section == "stderr" && key == "from") {
            stderrNodes[current_name].from = value;
            current_section = "";
            current_name = "";
        } else if (current_section == "file" && key == "name") {
            fileNodes[current_name].filename = value;
            current_section = "";
            current_name = "";
        }
        // General conditions
        else if (key == "node") {
            current_section = "node";
            current_name = value;
            Node node;
            node.name = current_name;
            node.command = "";
            nodes[current_name] = node;
        } else if (key == "pipe") {
            current_section = "pipe";
            current_name = value;
            Pipe pipe_obj;
            pipe_obj.name = current_name;
            pipe_obj.from = "";
            pipe_obj.to = "";
            pipes[current_name] = pipe_obj;
        } else if (key == "concatenate") {
            current_section = "concatenate";
            current_name = value;
            Concatenate concat;
            concat.name = current_name;
            concatenates[current_name] = concat;
        } else if (key == "stderr") {
            current_section = "stderr";
            current_name = value;
            StderrNode stderrNode;
            stderrNode.name = current_name;
            stderrNode.from = "";
            stderrNodes[current_name] = stderrNode;
        } else if (key == "file") {
            current_section = "file";
            current_name = value;
            FileNode fileNode;
            fileNode.name = current_name;
            fileNode.filename = "";
            fileNodes[current_name] = fileNode;
        } else {
            // Unknown key or outside of a section
        }
    }
    infile.close();
}

void executeItem(const string& itemName, int input_fd, int output_fd);

void executeNode(const Node& node, int input_fd, int output_fd, int error_fd = STDERR_FILENO) {
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
        if (error_fd != STDERR_FILENO) {
            dup2(error_fd, STDERR_FILENO);
            close(error_fd);
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
        if (error_fd != STDERR_FILENO) {
            close(error_fd);
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
        executeItem(pipe_obj.from, input_fd, pipefd[1]);
        exit(EXIT_SUCCESS);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        // Second child process ('to' item)
        close(pipefd[1]); // Close unused write end
        executeItem(pipe_obj.to, pipefd[0], output_fd);
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
    // Temporary storage to hold concatenated results
    string concatenated_output;

    for (const auto& partName : concat.part_names) {
        int pipefd[2];
        if (pipe(pipefd) == -1) {
            perror("pipe failed");
            exit(EXIT_FAILURE);
        }

        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            close(pipefd[0]); // Close read end of the pipe
            executeItem(partName, input_fd, pipefd[1]);
            close(pipefd[1]); // Close write end of the pipe
            exit(EXIT_SUCCESS);
        } else if (pid > 0) {
            // Parent process
            close(pipefd[1]); // Close write end

            // Read output from the child process and append to the buffer
            char buffer[4096];
            ssize_t bytes_read;
            while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
                concatenated_output.append(buffer, bytes_read);
            }
            close(pipefd[0]); // Close read end of the pipe
            waitpid(pid, NULL, 0); // Wait for the child process to finish
        } else {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }
    }

    // Write the concatenated result to the final output
    write(output_fd, concatenated_output.c_str(), concatenated_output.size());
}


void executeStderr(const StderrNode& stderrNode, int input_fd, int output_fd) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe failed");
        exit(EXIT_FAILURE);
    }

    // Execute the 'from' node and redirect its stderr to pipefd[1]
    if (nodes.find(stderrNode.from) != nodes.end()) {
        executeNode(nodes[stderrNode.from], input_fd, STDOUT_FILENO, pipefd[1]);
    } else {
        cerr << "Error: Node '" << stderrNode.from << "' not found for stderr" << endl;
        exit(EXIT_FAILURE);
    }

    close(pipefd[1]); // Close write end

    // Read from pipefd[0] and write to output_fd
    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        write(output_fd, buffer, bytes_read);
    }
    close(pipefd[0]);
}

void executeFileNode(const FileNode& fileNode, int input_fd, int output_fd) {
    if (input_fd == STDIN_FILENO && output_fd == STDOUT_FILENO) {
        cerr << "Error: File node '" << fileNode.name << "' is not connected properly" << endl;
        exit(EXIT_FAILURE);
    }

    if (input_fd == STDIN_FILENO) {
        // Input file: read from file and write to output_fd
        int file_fd = open(fileNode.filename.c_str(), O_RDONLY);
        if (file_fd == -1) {
            perror("Failed to open input file");
            exit(EXIT_FAILURE);
        }
        // Copy data from file_fd to output_fd
        char buffer[4096];
        ssize_t bytes_read;
        while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
            write(output_fd, buffer, bytes_read);
        }
        close(file_fd);
    } else {
        // Output file: read from input_fd, write to file, and pass data along if output_fd is specified
        int file_fd = open(fileNode.filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (file_fd == -1) {
            perror("Failed to open output file");
            exit(EXIT_FAILURE);
        }
        // Read from input_fd, write to file_fd, and to output_fd
        char buffer[4096];
        ssize_t bytes_read;
        while ((bytes_read = read(input_fd, buffer, sizeof(buffer))) > 0) {
            // Write to the file
            ssize_t bytes_written = write(file_fd, buffer, bytes_read);
            if (bytes_written == -1) {
                perror("Failed to write to file");
                exit(EXIT_FAILURE);
            }
            // Pass data along if needed
            if (output_fd != STDOUT_FILENO) {
                bytes_written = write(output_fd, buffer, bytes_read);
                if (bytes_written == -1) {
                    perror("Failed to write to output");
                    exit(EXIT_FAILURE);
                }
            }
        }
        close(file_fd);
    }
    if (input_fd != STDIN_FILENO) {
        close(input_fd);
    }
    if (output_fd != STDOUT_FILENO) {
        close(output_fd);
    }
}

void executeItem(const string& itemName, int input_fd, int output_fd) {
    if (nodes.find(itemName) != nodes.end()) {
        executeNode(nodes[itemName], input_fd, output_fd);
    } else if (pipes.find(itemName) != pipes.end()) {
        executePipe(pipes[itemName], input_fd, output_fd);
    } else if (concatenates.find(itemName) != concatenates.end()) {
        executeConcatenate(concatenates[itemName], input_fd, output_fd);
    } else if (stderrNodes.find(itemName) != stderrNodes.end()) {
        executeStderr(stderrNodes[itemName], input_fd, output_fd);
    } else if (fileNodes.find(itemName) != fileNodes.end()) {
        executeFileNode(fileNodes[itemName], input_fd, output_fd);
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
        concatenates.find(action_name) != concatenates.end() ||
        stderrNodes.find(action_name) != stderrNodes.end() ||
        fileNodes.find(action_name) != fileNodes.end()) {
        executeItem(action_name, STDIN_FILENO, STDOUT_FILENO);
    } else {
        cerr << "Error: Action '" << action_name << "' not found" << endl;
        exit(EXIT_FAILURE);
    }

    return 0;
}
