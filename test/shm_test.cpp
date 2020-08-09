#include "Interface.h"
#include "Utility.h"
#include "ShmInterface.h"

#include "plf_nanotimer/plf_nanotimer.h"

#ifdef _WIN32
# define APPNAME "shm_test.exe"
# include <Windows.h>
#else
# define APPNAME "shm_test"
# include <unistd.h>
# include <stdio.h>
# include <dlfcn.h>
# include <sys/wait.h>
#endif

#include <cstring>
#include <thread>

using namespace vst;

#define TEST_QUEUE 1
#define TEST_QUEUE_COUNT 100
#define TEST_QUEUE_BUFSIZE 256

#define TEST_REQUEST 1
#define TEST_REQUEST_BUFSIZE 512

#define TEST_BENCHMARK 1
#define TEST_BENCHMARK_COUNT 20

void sleep_ms(int ms){
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void server_test_queue(ShmInterface& shm){
    LOG_VERBOSE("---");
    LOG_VERBOSE("test request");
    LOG_VERBOSE("---");

    auto& channel = shm.getChannel(0);
    channel.clear();
    LOG_VERBOSE("server: channel " << channel.name());

    for (int i = 0; i < TEST_QUEUE_COUNT; ++i){
       char buf[64];
       snprintf(buf, sizeof(buf), "msg %d", i+1);
       if (channel.writeMessage(buf, strlen(buf) + 1)){
           LOG_VERBOSE("server: write message " << buf);
           // channel.post();
           shm.getChannel(1).post();
       } else {
           LOG_ERROR("server: couldn't write message " << buf);
       }

       sleep_ms(1); // prevent queue overflow
    }

    const char *quit = "quit";
    channel.writeMessage(quit, strlen(quit) + 1);
    // channel.post();
    shm.getChannel(1).post();

    // wait for child to finish. we'd better properly synchronize.
    sleep_ms(500);
}

void client_test_queue(ShmInterface& shm){
    auto& channel = shm.getChannel(0);
    LOG_VERBOSE("client: channel " << channel.name());

    int count = 0;
    for (;;){
        char buf[64];
        size_t size = sizeof(buf);
        while (channel.readMessage(buf, size)){
            LOG_VERBOSE("client: got message " << buf);
            if (!strcmp(buf, "quit")){
                LOG_VERBOSE("---");
                LOG_VERBOSE("client: got " << count << " messages");
                return;
            } else {
                count++;
            }
        }
        if (size > sizeof(buf)){
            LOG_ERROR("client: couldn't read message");
        } else {
            LOG_VERBOSE("client: waiting for message");
        }

    #if 0
        sleep_ms(1);
    #else
        channel.wait();
    #endif
    }
}

void server_test_request(ShmInterface& shm){
    LOG_VERBOSE("---");
    LOG_VERBOSE("test request");
    LOG_VERBOSE("---");

    auto& channel = shm.getChannel(1);
    channel.clear();
    LOG_VERBOSE("server: channel " << channel.name());
    // post message
    const char* msg[] = { "testing", "shared", "memory", "interface" };
    for (int i = 0; i < 4; ++i){
        LOG_VERBOSE("server: add msg: " << msg[i]);
        channel.addMessage(msg[i], strlen(msg[i]) + 1);
    }
    LOG_VERBOSE("server: send msg");
    channel.post();
    // wait for reply
    LOG_VERBOSE("server: wait for reply");

    channel.waitReply();
    const char *reply;
    size_t replySize;
    channel.getMessage(reply, replySize);

    LOG_VERBOSE("server: got reply: " << reply);
}

void client_test_request(ShmInterface& shm){
    auto& channel = shm.getChannel(1);

    LOG_VERBOSE("client: channel " << channel.name());
    // wait for messages
    LOG_VERBOSE("client: wait for message");
    channel.wait();
    for (int i = 0; i < 4; ++i){
        const char *msg;
        size_t msgSize;
        channel.getMessage(msg, msgSize);
        LOG_VERBOSE("client: got message: " << msg);
    }

    // std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // post reply
    auto reply = "ok";
    LOG_VERBOSE("client: send reply: " << reply);
    channel.clear();
    channel.addMessage(reply, strlen(reply) + 1);
    channel.postReply();
}

void server_benchmark(ShmInterface& shm){
    LOG_VERBOSE("---");
    LOG_VERBOSE("test benchmark");
    LOG_VERBOSE("---");

    auto& channel = shm.getChannel(1);
    LOG_VERBOSE("server: channel " << channel.name());

    plf::nanotimer timer;
    timer.start();

    {
        auto t1 = timer.get_elapsed_us();
        auto t2 = timer.get_elapsed_us();
        LOG_VERBOSE("server: empty interval: " << (t2 - t1) << " us");
    }


    for (int i = 0; i < TEST_BENCHMARK_COUNT; ++i){
        auto t1 = timer.get_elapsed_us();
        channel.clear();
        // post message
        auto msg = "test";
        channel.addMessage(msg, strlen(msg) + 1);
        auto t2 = timer.get_elapsed_us();
        channel.post();
        // wait for reply
        channel.waitReply();
        auto t3 = timer.get_elapsed_us();
        const char *reply;
        size_t replySize;
        channel.getMessage(reply, replySize);
        auto t4 = timer.get_elapsed_us();

        LOG_VERBOSE("server: full delta = " << (t4 - t1) << " us, "
                    << "wait delta = " << (t3 - t2) << " us");

        sleep_ms(1);
    }
}

void client_benchmark(ShmInterface& shm){
    auto& channel = shm.getChannel(1);

    LOG_VERBOSE("client: channel " << channel.name());

    for (int i = 0; i < TEST_BENCHMARK_COUNT; ++i){
        // wait for message
        channel.wait();
        const char *msg;
        size_t msgSize;
        channel.getMessage(msg, msgSize);

        // post reply
        auto reply = "ok";
        channel.clear();
        channel.addMessage(reply, strlen(reply) + 1);
        channel.postReply();
    }
}

int server_run(){
    LOG_VERBOSE("---");
    LOG_VERBOSE("server: start");
    LOG_VERBOSE("---");
    ShmInterface shm;
    shm.addChannel(ShmChannel::Queue, TEST_QUEUE_BUFSIZE, "queue");
    shm.addChannel(ShmChannel::Request, TEST_REQUEST_BUFSIZE, "request");
    shm.create();

    LOG_VERBOSE("server: created shared memory interface " << shm.path());

    // spawn child process
#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    char cmdLine[256];
    snprintf(cmdLine, sizeof(cmdLine), "%s %s", APPNAME, shm.path().c_str());

    if (!CreateProcessA(NULL, cmdLine, NULL, NULL,
                        TRUE, 0, NULL, NULL, &si, &pi)){
        throw Error(Error::SystemError, "CreateProcess() failed!");
    }
#else
    // fork
    pid_t pid = fork();
    if (pid == -1) {
        throw Error(Error::SystemError, "fork() failed!");
    } else if (pid == 0) {
        // child process
        if (execl(APPNAME, APPNAME, shm.path().c_str(), nullptr) < 0){
            throw Error(Error::SystemError, "execl() failed!");
        }
    }
    // continue with parent process
#endif

    sleep_ms(500);

#if TEST_QUEUE
    server_test_queue(shm);
#endif
#if TEST_REQUEST
    server_test_request(shm);
#endif
#if TEST_BENCHMARK
    server_benchmark(shm);
#endif

#ifdef _WIN32
    DWORD code = -1;
    if (WaitForSingleObject(pi.hProcess, INFINITE) != 0){
        throw Error(Error::SystemError, "WaitForSingleObject() failed!");
    }
    if (!GetExitCodeProcess(pi.hProcess, &code)){
        throw Error(Error::SystemError, "GetExitCodeProcess() failed!");
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    int code = -1;
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    }
#endif
    LOG_VERBOSE("child process finished with exit code " << code);

    return EXIT_SUCCESS;
}

int client_run(const char *path){
    LOG_VERBOSE("---");
    LOG_VERBOSE("client: start");
    LOG_VERBOSE("---");

    ShmInterface shm;
    shm.connect(path);

    LOG_VERBOSE("client: connected to shared memory interface " << path);

#if TEST_QUEUE
    client_test_queue(shm);
#endif
#if TEST_REQUEST
    client_test_request(shm);
#endif
#if TEST_BENCHMARK
    client_benchmark(shm);
#endif

    return EXIT_SUCCESS;
}

int main(int argc, const char *argv[]){
    setProcessPriority(Priority::High);
    setThreadPriority(Priority::High);

    try {
        if (argc > 1){
            return client_run(argv[1]);
        } else {
            return server_run();
        }
    } catch (const std::exception& e){
        LOG_ERROR(e.what());
        return EXIT_FAILURE;
    }
}
