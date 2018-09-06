#include "fimd_impl.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <functional>
#include <future>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <glog/logging.h>
#include <grpc/grpc.h>
#include <grpc++/server_context.h>

#include "fimd_util.h"
extern "C" {
#include "lib/fimnotify.h"
}

namespace fimd {
std::string FimdImpl::DEFAULT_FORMAT = "{event} {ftype} '{path}{sep}{file}' ({pod}:{node})";

grpc::Status FimdImpl::CreateWatch(grpc::ServerContext *context, const fim::FimdConfig *request, fim::FimdHandle *response) {
    auto pids = getPidsFromRequest(request);
    if (!pids.size()) {
        return grpc::Status::CANCELLED;
    }

    // find existing watcher by pid in case we need to update
    // inotify_add_watcher is designed to both add and modify depending
    // on if a fd exists already for this path
    auto watcher = findFimdWatcherByPids(request->nodename(), pids);
    if (watcher == nullptr) {
        LOG(INFO) << "Starting inotify watcher...";
    } else {
        LOG(INFO) << "Updating inotify watcher...";

        // stop existing watcher polling
        sendKillSignalToWatcher(watcher);
        watcher->clear_processeventfd();
    }

    response->set_nodename(request->nodename().c_str());
    response->set_podname(request->podname().c_str());
    response->set_mqfd(static_cast<google::protobuf::int32>(createMessageQueue(request->logformat(),
        request->nodename(), request->podname(), (watcher != nullptr))));

    for_each(pids.cbegin(), pids.cend(), [&](const int pid) {
        for_each(request->subject().cbegin(), request->subject().cend(), [&](const fim::FimWatcherSubject subject) {
            // @TODO: check if any watchers are started, if not, don't add to response
            createInotifyWatcher(subject, pid, getPathArrayFromSubject(pid, subject),
                getEventMaskFromSubject(subject), response->mutable_processeventfd());
        });
        response->add_pid(pid);
    });

    if (watcher == nullptr) {
        // store new watcher
        watchers_.push_back(std::make_shared<fim::FimdHandle>(*response));
    } else {
        std::for_each(response->processeventfd().cbegin(), response->processeventfd().cend(), [&](const int processfd) {
            watcher->add_processeventfd(processfd);
        });
    }

    return grpc::Status::OK;
}

grpc::Status FimdImpl::DestroyWatch(grpc::ServerContext *context, const fim::FimdConfig *request, fim::Empty *response) {
    auto pids = getPidsFromRequest(request);
    if (!pids.size()) {
        return grpc::Status::CANCELLED;
    }

    LOG(INFO) << "Stopping inotify watcher...";

    auto watcher = findFimdWatcherByPids(request->nodename(), pids);
    if (watcher != nullptr) {
        // stop existing message queue
        sendExitMessageToMessageQueue(watcher);
        // stop existing watcher polling
        sendKillSignalToWatcher(watcher);
    }
    watchers_.erase(remove(watchers_.begin(), watchers_.end(), watcher), watchers_.end());

    return grpc::Status::OK;
}

std::vector<int> FimdImpl::getPidsFromRequest(const fim::FimdConfig *request) {
    std::vector<int> pids;
    std::for_each(request->containerid().cbegin(), request->containerid().cend(), [&](const std::string containerId) {
        int pid = FimdUtil::getPidForContainer(cleanContainerId(containerId));
        if (pid) {
            pids.push_back(pid);
        }
    });
    return pids;
}

std::shared_ptr<fim::FimdHandle> FimdImpl::findFimdWatcherByPids(const std::string nodeName, const std::vector<int> pids) {
    auto it = find_if(watchers_.cbegin(), watchers_.cend(), [&](std::shared_ptr<fim::FimdHandle> watcher) {
        bool foundPid;
        for (auto pid = pids.cbegin(); pid != pids.cend(); ++pid) {
            auto watcherPid = std::find_if(watcher->pid().cbegin(), watcher->pid().cend(),
                [&](int p) { return p == *pid; });
            foundPid = watcherPid != watcher->pid().cend();
        }
        return watcher->nodename() == nodeName && foundPid;
    });
    if (it != watchers_.cend()) {
        return *it;
    }
    return nullptr;
}

char **FimdImpl::getPathArrayFromSubject(const int pid, const fim::FimWatcherSubject subject) {
    std::vector<std::string> pathvec;
    std::for_each(subject.path().cbegin(), subject.path().cend(), [&](std::string path) {
        std::stringstream ss;
        ss << "/proc/" << pid << "/root" << path.c_str();
        pathvec.push_back(ss.str());
    });

    char **patharr = new char *[pathvec.size()];
    for(size_t i = 0; i < pathvec.size(); ++i) {
        patharr[i] = new char[pathvec[i].size() + 1];
        strcpy(patharr[i], pathvec[i].c_str());
    }
    return patharr;
}

uint32_t FimdImpl::getEventMaskFromSubject(const fim::FimWatcherSubject subject) {
    // @TODO: document this
    uint32_t mask = 0; //IN_DONT_FOLLOW;
    std::for_each(subject.event().cbegin(), subject.event().cend(), [&](std::string event) {
        const char *evt = event.c_str();
        if (strcmp(evt, "all") == 0)         mask |= IN_ALL_EVENTS;
        else if (strcmp(evt, "access") == 0) mask |= IN_ACCESS;
        else if (strcmp(evt, "modify") == 0) mask |= IN_MODIFY;
        else if (strcmp(evt, "attrib") == 0) mask |= IN_ATTRIB;
        else if (strcmp(evt, "open") == 0)   mask |= IN_OPEN;
        else if (strcmp(evt, "close") == 0)  mask |= IN_CLOSE;
        else if (strcmp(evt, "create") == 0) mask |= IN_CREATE;
        else if (strcmp(evt, "delete") == 0) mask |= IN_DELETE;
        else if (strcmp(evt, "move") == 0)   mask |= IN_MOVE;
    });
    return mask;
}

void FimdImpl::createInotifyWatcher(const fim::FimWatcherSubject subject, const int pid, char **patharr, uint32_t event_mask,
    google::protobuf::RepeatedField<google::protobuf::int32> *eventProcessfds) {
    // create anonymous pipe to communicate with inotify watcher
    int processfd = eventfd(0, EFD_CLOEXEC);
    if (processfd == EOF) {
        return;
    }
    eventProcessfds->Add(processfd);

    std::packaged_task<int(int, int, char **, uint32_t, bool, int, mqd_t)> task(start_inotify_watcher);
    std::shared_future<int> result(task.get_future());
    std::thread taskThread(std::move(task), pid, subject.path_size(), static_cast<char **>(patharr),
        static_cast<uint32_t>(event_mask), subject.recursive(), processfd, mq_);
    // start as daemon process
    taskThread.detach();

    // @TODO: document this
    std::thread cleanupThread([&](std::shared_future<int> res) {
        std::future_status status;
        do {
            status = res.wait_for(std::chrono::seconds(1));
        } while (status != std::future_status::ready);

        if (res.valid() &&
            res.get() != EXIT_SUCCESS) {
            eraseEventProcessfd(eventProcessfds, processfd);
        }
    }, result);
    cleanupThread.detach();
}

mqd_t FimdImpl::createMessageQueue(const std::string logFormat, const std::string nodeName, const std::string podName, bool recreate) {
    mq_attr attr;
    // initialize the queue attributes
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = MQ_MAX_SIZE;
    attr.mq_curmsgs = 0;

    if (recreate) {
        mq_close(mq_);
        mq_unlink(MQ_QUEUE_NAME);
    }

    // create the message queue
    mq_ = mq_open(MQ_QUEUE_NAME, O_CREAT | O_CLOEXEC | O_RDWR, S_IRUSR | S_IWUSR, &attr);
    if (mq_ == EOF) {
#if DEBUG
        perror("mq_open");
#endif
        return EOF;
    }

    // start message queue
    std::packaged_task<void(const std::string, const std::string, const std::string, mqd_t)> queue(startMessageQueue);
    std::thread queueThread(move(queue), logFormat, nodeName, podName, mq_);
    // start as daemon process
    queueThread.detach();

    return mq_;
}

void FimdImpl::startMessageQueue(const std::string logFormat, const std::string nodeName, const std::string podName, mqd_t mq) {
    bool done = false;
    do {
        char buffer[MQ_MAX_SIZE + 1];
        ssize_t bytes_read = mq_receive(mq, buffer, MQ_MAX_SIZE, NULL);
        buffer[bytes_read] = '\0';
        if (bytes_read == EOF) {
            continue;
        }

        if (!strncmp(buffer, MQ_EXIT_MESSAGE, strlen(MQ_EXIT_MESSAGE))) {
            done = true;
        } else {
            fimwatch_event *fwevent = reinterpret_cast<struct fimwatch_event *>(buffer);
            std::regex proc_regex("/proc/[0-9]+/root");

            std::string mask_str;
            if (fwevent->event_mask & IN_ACCESS)             mask_str = "IN_ACCESS";
            else if (fwevent->event_mask & IN_MODIFY)        mask_str = "IN_MODIFY";
            else if (fwevent->event_mask & IN_ATTRIB)        mask_str = "IN_ATTRIB";
            else if (fwevent->event_mask & IN_OPEN)          mask_str = "IN_OPEN";
            else if (fwevent->event_mask & IN_CLOSE_WRITE)   mask_str = "IN_CLOSE_WRITE";
            else if (fwevent->event_mask & IN_CLOSE_NOWRITE) mask_str = "IN_CLOSE_NOWRITE";
            else if (fwevent->event_mask & IN_CREATE)        mask_str = "IN_CREATE";
            else if (fwevent->event_mask & IN_DELETE)        mask_str = "IN_DELETE";
            else if (fwevent->event_mask & IN_DELETE_SELF)   mask_str = "IN_DELETE_SELF";
            else if (fwevent->event_mask & IN_MOVED_FROM)    mask_str = "IN_MOVED_FROM";
            else if (fwevent->event_mask & IN_MOVED_TO)      mask_str = "IN_MOVED_TO";
            else if (fwevent->event_mask & IN_MOVE_SELF)     mask_str = "IN_MOVE_SELF";

            fmt::memory_buffer out;
            try {
                fmt::format_to(out, logFormat != "" ? logFormat : FimdImpl::DEFAULT_FORMAT,
                    fmt::arg("event", mask_str),
                    fmt::arg("ftype", fwevent->is_dir ? "directory" : "file"),
                    fmt::arg("path", std::regex_replace(fwevent->path_name, proc_regex, "")),
                    fmt::arg("file", fwevent->file_name),
                    fmt::arg("sep", fwevent->file_name != "" ? "/" : ""),
                    fmt::arg("pod", podName),
                    fmt::arg("node", nodeName));
                LOG(INFO) << fmt::to_string(out);
            } catch(const std::exception &e) {
                LOG(WARNING) << "Malformed FimWatcher `.spec.logFormat`: \"" << e.what() << "\"";
            }
        }
    } while (!done);

    mq_close(mq);
    mq_unlink(MQ_QUEUE_NAME);
}

void FimdImpl::sendKillSignalToWatcher(std::shared_ptr<fim::FimdHandle> watcher) {
    // kill existing watcher polls
    uint64_t value = FIMNOTIFY_KILL;
    std::for_each(watcher->processeventfd().cbegin(), watcher->processeventfd().cend(), [&](const int processfd) {
        if (write(processfd, &value, sizeof(value)) == EOF) {
            // do stuff
        }
        eraseEventProcessfd(watcher->mutable_processeventfd(), processfd);
    });
}

void FimdImpl::eraseEventProcessfd(google::protobuf::RepeatedField<google::protobuf::int32> *eventProcessfds, const int processfd) {
    for (auto it = eventProcessfds->cbegin(); it != eventProcessfds->cend(); ++it) {
        if (*it == processfd) {
            eventProcessfds->erase(it);
            break;
        }
    }
}

void FimdImpl::sendExitMessageToMessageQueue(std::shared_ptr<fim::FimdHandle> watcher) {
    // @TODO: document this
    if (mq_send(watcher->mqfd(), MQ_EXIT_MESSAGE, strlen(MQ_EXIT_MESSAGE), 1) == EOF) {
        // do stuff
    }
}
} // namespace fimd
