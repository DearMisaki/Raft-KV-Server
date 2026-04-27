#include "raft.h"
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include "config.h"
#include "util.h"

void Raft::AppendEntries1(const raftRpcProctoc::AppendEntriesArgs* args, raftRpcProctoc::AppendEntriesReply* reply) {
  std::lock_guard<std::mutex> locker(m_mtx);
  reply->set_appstate(AppNormal);  // 能接收到代表网络是正常的

  // leader 的任期更小，应该立刻回退 follower
  if (args->term() < m_currentTerm) {
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    reply->set_updatenextindex(-100);  // 论文中：让领导人可以及时更新自己
    DPrintf("[func-AppendEntries-rf{%d}] 拒绝了 因为Leader{%d}的term{%v}< rf{%d}.term{%d}\n", m_me, args->leaderid(),
            args->term(), m_me, m_currentTerm);
    return;  // 注意从过期的领导人收到消息不要重设定时器
  }

  // 由于这个局部变量创建在锁之后，因此执行persist的时候应该也是拿到锁的.
  DEFER { persist(); };  // 由于这个局部变量创建在锁之后，因此执行persist的时候应该也是拿到锁的.

  if (args->term() > m_currentTerm) {
    // 三变 ,防止遗漏，无论什么时候都是三变
    // DPrintf("[func-AppendEntries-rf{%v} ] 变成follower且更新term 因为Leader{%v}的term{%v}> rf{%v}.term{%v}\n",
    // rf.me, args.LeaderId, args.Term, rf.me, rf.currentTerm)
    m_status = Follower;
    m_currentTerm = args->term();
    m_votedFor = -1;  // 这里设置成-1有意义，如果突然宕机然后上线理论上是可以投票的
                      // 这里可不返回，应该改成让改节点尝试接收日志
                      // 如果是领导人和candidate突然转到Follower好像也不用其他操作
    // 如果本来就是Follower，那么其term变化，相当于“不言自明”的换了追随的对象，因为原来的leader的term更小，是不会再接收其消息了
  }
  myAssert(args->term() == m_currentTerm, format("assert {args.Term == rf.currentTerm} fail"));
  // 如果发生网络分区，那么candidate可能会收到同一个term的leader的消息，要转变为Follower，为了和上面，因此直接写
  m_status = Follower;  // 这里是有必要的，因为如果candidate收到同一个term的leader的AE，需要变成follower
  // term相等
  m_lastResetElectionTime = now();
  //  DPrintf("[	AppendEntries-func-rf(%v)		] 重置了选举超时定时器\n", rf.me);

  // 不能无脑的从prevlogIndex开始阶段日志，因为rpc可能会延迟，导致发过来的log是很久之前的

  // leader 上一条日志 > follower
  if (args->prevlogindex() > getLastLogIndex()) {
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    reply->set_updatenextindex(getLastLogIndex() + 1);
    //  DPrintf("[func-AppendEntries-rf{%v}] 拒绝了节点{%v}，因为日志太新,args.PrevLogIndex{%v} >
    //  lastLogIndex{%v}，返回值：{%v}\n", rf.me, args.LeaderId, args.PrevLogIndex, rf.getLastLogIndex(), reply)
    return;
  } else if (args->prevlogindex() < m_lastSnapshotIncludeIndex) {
    // 如果prevlogIndex还没有更上快照
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    reply->set_updatenextindex(
        m_lastSnapshotIncludeIndex +
        1);  // todo 如果想直接弄到最新好像不对，因为是从后慢慢往前匹配的，这里不匹配说明后面的都不匹配
    //  DPrintf("[func-AppendEntries-rf{%v}] 拒绝了节点{%v}，因为log太老，返回值：{%v}\n", rf.me, args.LeaderId,
    //  reply) return
  }
  //	本机日志有那么长，冲突(same index,different term),截断日志
  // 注意：这里目前当args.PrevLogIndex == rf.lastSnapshotIncludeIndex与不等的时候要分开考虑，可以看看能不能优化这块
  if (matchLog(args->prevlogindex(), args->prevlogterm())) {
    //	todo：	整理logs
    // ，不能直接截断，必须一个一个检查，因为发送来的log可能是之前的，直接截断可能导致“取回”已经在follower日志中的条目
    // 那意思是不是可能会有一段发来的AE中的logs中前半是匹配的，后半是不匹配的，这种应该：1.follower如何处理？ 2.如何给leader回复
    // 3. leader如何处理

    for (int i = 0; i < args->entries_size(); i++) {
      auto log = args->entries(i);
      if (log.logindex() > getLastLogIndex()) {
        // 超过就直接添加日志
        m_logs.push_back(log);
      } else {
        // 没超过就比较是否匹配，不匹配再更新，而不是直接截断
        //  todo ： 这里可以改进为比较对应logIndex位置的term是否相等，term相等就代表匹配
        //   todo：这个地方放出来会出问题,按理说index相同，term相同，log也应该相同才对
        //  rf.logs[entry.Index-firstIndex].Term ?= entry.Term

        if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() == log.logterm() &&
            m_logs[getSlicesIndexFromLogIndex(log.logindex())].command() != log.command()) {
          // 相同位置的log ，其logTerm相等，但是命令却不相同，不符合raft的前向匹配，异常了！
          myAssert(false, format("[func-AppendEntries-rf{%d}] 两节点logIndex{%d}和term{%d}相同，但是其command{%d:%d}   "
                                 " {%d:%d}却不同！！\n",
                                 m_me, log.logindex(), log.logterm(), m_me,
                                 m_logs[getSlicesIndexFromLogIndex(log.logindex())].command(), args->leaderid(),
                                 log.command()));
        }
        if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() != log.logterm()) {
          // 不匹配就更新
          m_logs[getSlicesIndexFromLogIndex(log.logindex())] = log;
        }
      }
    }

    // 错误写法like：  rf.shrinkLogsToIndex(args.PrevLogIndex)
    // rf.logs = append(rf.logs, args.Entries...)
    // 因为可能会收到过期的log！！！ 因此这里是大于等于
    myAssert(
        getLastLogIndex() >= args->prevlogindex() + args->entries_size(),
        format("[func-AppendEntries1-rf{%d}]rf.getLastLogIndex(){%d} != args.PrevLogIndex{%d}+len(args.Entries){%d}",
               m_me, getLastLogIndex(), args->prevlogindex(), args->entries_size()));
    // if len(args.Entries) > 0 {
    //	fmt.Printf("[func-AppendEntries  rf:{%v}] ] : args.term:%v, rf.term:%v  ,rf.logs的长度：%v\n", rf.me,
    // args.Term,
    // rf.currentTerm, len(rf.logs))
    // }
    if (args->leadercommit() > m_commitIndex) {
      m_commitIndex = std::min(args->leadercommit(), getLastLogIndex());
      // 这个地方不能无脑跟上getLastLogIndex()，因为可能存在args->leadercommit()落后于 getLastLogIndex()的情况
      m_applyCond.notify_one();
    }

    // 领导会一次发送完所有的日志
    myAssert(getLastLogIndex() >= m_commitIndex,
             format("[func-AppendEntries1-rf{%d}]  rf.getLastLogIndex{%d} < rf.commitIndex{%d}", m_me,
                    getLastLogIndex(), m_commitIndex));
    reply->set_success(true);
    reply->set_term(m_currentTerm);

    //        DPrintf("[func-AppendEntries-rf{%v}] 接收了来自节点{%v}的log，当前lastLogIndex{%v}，返回值：{%v}\n",
    //        rf.me,
    //                args.LeaderId, rf.getLastLogIndex(), reply)

    return;
  } else {
    // 优化
    // PrevLogIndex 长度合适，但是不匹配，因此往前寻找 矛盾的term的第一个元素
    // 为什么该term的日志都是矛盾的呢？也不一定都是矛盾的，只是这么优化减少rpc而已
    // ？什么时候term会矛盾呢？很多情况，比如leader接收了日志之后马上就崩溃等等
    reply->set_updatenextindex(args->prevlogindex());

    for (int index = args->prevlogindex(); index >= m_lastSnapshotIncludeIndex; --index) {
      if (getLogTermFromLogIndex(index) != getLogTermFromLogIndex(args->prevlogindex())) {
        reply->set_updatenextindex(index + 1);
        break;
      }
    }
    reply->set_success(false);
    reply->set_term(m_currentTerm);
    // 对UpdateNextIndex待优化  todo  找到符合的term的最后一个
    //        DPrintf("[func-AppendEntries-rf{%v}]
    //        拒绝了节点{%v}，因为prevLodIndex{%v}的args.term{%v}不匹配当前节点的logterm{%v}，返回值：{%v}\n",
    //                rf.me, args.LeaderId, args.PrevLogIndex, args.PrevLogTerm,
    //                rf.logs[rf.getSlicesIndexFromLogIndex(args.PrevLogIndex)].LogTerm, reply)
    //        DPrintf("[func-AppendEntries-rf{%v}] 返回值: reply.UpdateNextIndex从{%v}优化到{%v}，优化了{%v}\n",
    //        rf.me,
    //                args.PrevLogIndex, reply.UpdateNextIndex, args.PrevLogIndex - reply.UpdateNextIndex) //
    //                很多都是优化了0
    return;
  }

  // fmt.Printf("[func-AppendEntries,rf{%v}]:len(rf.logs):%v, rf.commitIndex:%v\n", rf.me, len(rf.logs),
  // rf.commitIndex)
}

void Raft::applierTicker() {
  while (true) {
    std::vector<ApplyMsg> applyMsgs;

    {
      // 条件变量必须配合 std::unique_lock 使用
      std::unique_lock<std::mutex> lock(m_mtx);

      // 挂起当前协程/线程，释放锁。
      // 只有当 m_commitIndex > m_lastApplied 且收到 notify 时才会醒来并重新获得锁
      m_applyCond.wait(lock, [this]() { return m_commitIndex > m_lastApplied; });

      if (m_status == Leader) {
        DPrintf("[Raft::applierTicker() - raft{%d}] 被唤醒! m_lastApplied{%d} m_commitIndex{%d}", m_me, m_lastApplied,
                m_commitIndex);
      }

      // 醒来后，获取需要提交的日志序列
      applyMsgs = getApplyLogs();
    }  // 离开作用域，自动 unlock() 释放锁

    // 将日志推给 KV Server (这一步绝对不能拿锁，否则容易和 KV 层发生死锁)
    if (!applyMsgs.empty()) {
      DPrintf("[func- Raft::applierTicker()-raft{%d}] 向kvserver報告的applyMsgs長度爲：{%d}", m_me, applyMsgs.size());
    }

    for (auto& message : applyMsgs) {
      applyChan->Push(message);
    }
  }
}

bool Raft::CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot) {
  return true;
  //// Your code here (2D).
  // rf.mu.Lock()
  // defer rf.mu.Unlock()
  // DPrintf("{Node %v} service calls CondInstallSnapshot with lastIncludedTerm %v and lastIncludedIndex {%v} to check
  // whether snapshot is still valid in term %v", rf.me, lastIncludedTerm, lastIncludedIndex, rf.currentTerm)
  //// outdated snapshot
  // if lastIncludedIndex <= rf.commitIndex {
  //	return false
  // }
  //
  // lastLogIndex, _ := rf.getLastLogIndexAndTerm()
  // if lastIncludedIndex > lastLogIndex {
  //	rf.logs = make([]LogEntry, 0)
  // } else {
  //	rf.logs = rf.logs[rf.getSlicesIndexFromLogIndex(lastIncludedIndex)+1:]
  // }
  //// update dummy entry with lastIncludedTerm and lastIncludedIndex
  // rf.lastApplied, rf.commitIndex = lastIncludedIndex, lastIncludedIndex
  //
  // rf.persister.Save(rf.persistData(), snapshot)
  // return true
}

void Raft::doElection() {
  std::lock_guard<std::mutex> g(m_mtx);

  if (m_status == Leader) {
  }

  if (m_status != Leader) {
    DPrintf("[       ticker-func-rf(%d)              ]  选举定时器到期且不是leader，开始选举 \n", m_me);

    m_status = Candidate;
    // 开始选举就自增了 term
    m_currentTerm += 1;
    m_votedFor = m_me;

    persist();

    std::shared_ptr<int> votedNum = std::make_shared<int>(1);  // 使用 make_shared 函数初始化 !! 亮点

    m_lastResetElectionTime = now();

    for (int i = 0; i < m_peers.size(); i++) {
      if (i == m_me) {
        continue;
      }

      int lastLogIndex = -1, lastLogTerm = -1;
      getLastLogIndexAndTerm(&lastLogIndex, &lastLogTerm);  // 获取最后一个log的term和下标

      // RPC 调用的参数
      std::shared_ptr<raftRpcProctoc::RequestVoteArgs> requestVoteArgs =
          std::make_shared<raftRpcProctoc::RequestVoteArgs>();

      // 对应 raftRPC.proto 中设置的几个参数
      requestVoteArgs->set_term(m_currentTerm);
      requestVoteArgs->set_candidateid(m_me);
      requestVoteArgs->set_lastlogindex(lastLogIndex);
      requestVoteArgs->set_lastlogterm(lastLogTerm);
      auto requestVoteReply = std::make_shared<raftRpcProctoc::RequestVoteReply>();

      // 使用匿名函数执行避免其拿到锁

      std::thread t(&Raft::sendRequestVote, this, i, requestVoteArgs, requestVoteReply,
                    votedNum);  // 创建新线程并执行b函数，并传递参数
      t.detach();
    }
  }
}

void Raft::doHeartBeat() {
  std::lock_guard<std::mutex> g(m_mtx);

  // 提前返回，减少代码嵌套层级
  if (m_status != Leader) {
    return;
  }

  DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了且拿到mutex，开始发送AE\n", m_me);

  // 正确返回的节点数量，初始化为 1 (Leader 自己默认成功)
  auto appendNums = std::make_shared<int>(1);

  for (int i = 0; i < m_peers.size(); i++) {
    if (i == m_me) {
      continue;
    }

    DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] 向节点 {%d} 发送心跳/AE\n", m_me, i);
    myAssert(m_nextIndex[i] >= 1, format("rf.nextIndex[%d] = {%d}", i, m_nextIndex[i]));

    // 1. 发送快照分支
    if (m_nextIndex[i] <= m_lastSnapshotIncludeIndex) {
      // 获取安全的智能指针，防止后台任务执行时 Raft 对象被销毁
      auto self = shared_from_this();

      // 用 Lambda 封装任务，方便投递给协程调度器或线程池
      auto snapshotTask = [self, i]() { self->leaderSendSnapShot(i); };

      m_ioManager->scheduler(snapshotTask);
      continue;
    }

    // 2. 组装普通的 AppendEntries (心跳或日志)
    int preLogIndex = -1;
    int prevLogTerm = -1;
    getPrevLogInfo(i, &preLogIndex, &prevLogTerm);

    auto appendEntriesArgs = std::make_shared<raftRpcProctoc::AppendEntriesArgs>();

    appendEntriesArgs->set_term(m_currentTerm);
    appendEntriesArgs->set_leaderid(m_me);
    appendEntriesArgs->set_prevlogindex(preLogIndex);
    appendEntriesArgs->set_prevlogterm(prevLogTerm);
    appendEntriesArgs->set_leadercommit(m_commitIndex);

    // 上一次的 logindex != 归档的 logindex，代表归档后存在更新的 log
    // 此时发送增量数据
    if (preLogIndex != m_lastSnapshotIncludeIndex) {
      // 这个 prelogindx 是全局的，包括了归档里面的编号
      // 所以要转换到内存中的编号
      int startIndex = getSlicesIndexFromLogIndex(preLogIndex) + 1;

      for (int j = startIndex; j < m_logs.size(); ++j) {
        *appendEntriesArgs->add_entries() = m_logs[j];
      }
      // preLogIndex == m_lastSnapshotIncludeIndex，那么只需要遍历整个 m_log
    } else {
      for (const auto& item : m_logs) {
        *appendEntriesArgs->add_entries() = item;
      }
    }

    // m_log中的最后一个日志
    int lastLogIndex = getLastLogIndex();

    myAssert(appendEntriesArgs->prevlogindex() + appendEntriesArgs->entries_size() == lastLogIndex,
             format("appendEntriesArgs.PrevLogIndex{%d}+len(Entries){%d} != lastLogIndex{%d}",
                    appendEntriesArgs->prevlogindex(), appendEntriesArgs->entries_size(), lastLogIndex));

    auto appendEntriesReply = std::make_shared<raftRpcProctoc::AppendEntriesReply>();
    appendEntriesReply->set_appstate(Disconnected);

    // 3. 异步发送 AppendEntries
    auto self = shared_from_this();  // 获取安全的智能指针

    // 用 Lambda 捕获所有上下文，任务逻辑更加内聚
    auto appendTask = [self, i, appendEntriesArgs, appendEntriesReply, appendNums]() {
      self->sendAppendEntries(i, appendEntriesArgs, appendEntriesReply, appendNums);
    };

    m_ioManager->scheduler(appendTask);
  }

  // 重置心跳定时器
  m_lastResetHearBeatTime = now();
}

/*
 * 由 init() 注册到协程中
 */
void Raft::electionTimeOutTicker() {
  // Check if a Leader election should be started.
  while (true) {
    // 对于 leader 来说，一直睡眠就好了，但是要折中考虑睡眠时间，避免带来更多的 CPU 压力
    while (m_status == Leader) {
      usleep(HeartBeatTimeout);
    }

    std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
    std::chrono::system_clock::time_point wakeTime{};
    // 这里加锁避免和其他协程竞争 m_lastResetElectionTime
    {
      m_mtx.lock();
      wakeTime = now();
      // 上次重置的选举时间 - 当前时间，肯定是负数，如果加上随机超时时间 > 0，则代表还没超时，还应该继续睡
      suitableSleepTime = getRandomizedElectionTimeout() + m_lastResetElectionTime - wakeTime;
      m_mtx.unlock();
    }

    if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) {
      auto start = std::chrono::steady_clock::now();

      usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());

      auto end = std::chrono::steady_clock::now();

      std::chrono::duration<double, std::milli> duration = end - start;

      std::cout << "\033[1;35m electionTimeOutTicker();函数设置睡眠时间为: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                << std::endl;
      std::cout << "\033[1;35m electionTimeOutTicker();函数实际睡眠时间为: " << duration.count() << " 毫秒\033[0m"
                << std::endl;
    }

    // 大于 0 代表，睡眠的这段时间，其他协程重新设置了 m_lastResetElectionTime
    // 那么也就代表收到了 leader 发来的心跳 RPC
    if (std::chrono::duration<double, std::milli>(m_lastResetElectionTime - wakeTime).count() > 0) {
      continue;
    }
    doElection();
  }
}

std::vector<ApplyMsg> Raft::getApplyLogs() {
  std::vector<ApplyMsg> applyMsgs;
  myAssert(m_commitIndex <= getLastLogIndex(), format("[func-getApplyLogs-rf{%d}] commitIndex{%d} >getLastLogIndex{%d}",
                                                      m_me, m_commitIndex, getLastLogIndex()));

  while (m_lastApplied < m_commitIndex) {
    m_lastApplied++;
    myAssert(m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex() == m_lastApplied,
             format("rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogIndex{%d} != rf.lastApplied{%d} ",
                    m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex(), m_lastApplied));
    ApplyMsg applyMsg;
    applyMsg.CommandValid = true;
    applyMsg.SnapshotValid = false;
    applyMsg.Command = m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].command();
    applyMsg.CommandIndex = m_lastApplied;
    applyMsgs.emplace_back(applyMsg);
    //        DPrintf("[	applyLog func-rf{%v}	] apply Log,logIndex:%v  ，logTerm：{%v},command：{%v}\n",
    //        rf.me, rf.lastApplied, rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogTerm,
    //        rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].Command)
  }
  return applyMsgs;
}

// 获取新命令应该分配的Index
int Raft::getNewCommandIndex() {
  //	如果len(logs)==0,就为快照的index+1，否则为log最后一个日志+1
  auto lastLogIndex = getLastLogIndex();
  return lastLogIndex + 1;
}

// getPrevLogInfo
// leader调用，传入：服务器index，传出：发送的AE的preLogIndex和PrevLogTerm
void Raft::getPrevLogInfo(int server, int* preIndex, int* preTerm) {
  // logs长度为0返回0,0，不是0就根据nextIndex数组的数值返回
  if (m_nextIndex[server] == m_lastSnapshotIncludeIndex + 1) {
    // 要发送的日志是第一个日志，因此直接返回m_lastSnapshotIncludeIndex和m_lastSnapshotIncludeTerm
    *preIndex = m_lastSnapshotIncludeIndex;
    *preTerm = m_lastSnapshotIncludeTerm;
    return;
  }
  auto nextIndex = m_nextIndex[server];
  *preIndex = nextIndex - 1;
  *preTerm = m_logs[getSlicesIndexFromLogIndex(*preIndex)].logterm();
}

// GetState return currentTerm and whether this server
// believes it is the Leader.
void Raft::GetState(int* term, bool* isLeader) {
  m_mtx.lock();
  DEFER {
    // todo 暂时不清楚会不会导致死锁
    m_mtx.unlock();
  };

  // Your code here (2A).
  *term = m_currentTerm;
  *isLeader = (m_status == Leader);
}

void Raft::InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest* args,
                           raftRpcProctoc::InstallSnapshotResponse* reply) {
  m_mtx.lock();
  DEFER { m_mtx.unlock(); };
  if (args->term() < m_currentTerm) {
    reply->set_term(m_currentTerm);
    //        DPrintf("[func-InstallSnapshot-rf{%v}] leader{%v}.term{%v}<rf{%v}.term{%v} ", rf.me, args.LeaderId,
    //        args.Term, rf.me, rf.currentTerm)

    return;
  }
  if (args->term() > m_currentTerm) {
    // 后面两种情况都要接收日志
    m_currentTerm = args->term();
    m_votedFor = -1;
    m_status = Follower;
    persist();
  }
  m_status = Follower;
  m_lastResetElectionTime = now();
  // outdated snapshot
  if (args->lastsnapshotincludeindex() <= m_lastSnapshotIncludeIndex) {
    //        DPrintf("[func-InstallSnapshot-rf{%v}] leader{%v}.LastSnapShotIncludeIndex{%v} <=
    //        rf{%v}.lastSnapshotIncludeIndex{%v} ", rf.me, args.LeaderId, args.LastSnapShotIncludeIndex, rf.me,
    //        rf.lastSnapshotIncludeIndex)
    return;
  }
  // 截断日志，修改commitIndex和lastApplied
  // 截断日志包括：日志长了，截断一部分，日志短了，全部清空，其实两个是一种情况
  // 但是由于现在getSlicesIndexFromLogIndex的实现，不能传入不存在logIndex，否则会panic
  auto lastLogIndex = getLastLogIndex();

  if (lastLogIndex > args->lastsnapshotincludeindex()) {
    m_logs.erase(m_logs.begin(), m_logs.begin() + getSlicesIndexFromLogIndex(args->lastsnapshotincludeindex()) + 1);
  } else {
    m_logs.clear();
  }
  m_commitIndex = std::max(m_commitIndex, args->lastsnapshotincludeindex());
  m_lastApplied = std::max(m_lastApplied, args->lastsnapshotincludeindex());
  m_lastSnapshotIncludeIndex = args->lastsnapshotincludeindex();
  m_lastSnapshotIncludeTerm = args->lastsnapshotincludeterm();

  reply->set_term(m_currentTerm);
  ApplyMsg msg;
  msg.SnapshotValid = true;
  msg.Snapshot = args->data();
  msg.SnapshotTerm = args->lastsnapshotincludeterm();
  msg.SnapshotIndex = args->lastsnapshotincludeindex();

  std::thread t(&Raft::pushMsgToKvServer, this, msg);  // 创建新线程并执行b函数，并传递参数
  t.detach();
  // 看下这里能不能再优化
  //     DPrintf("[func-InstallSnapshot-rf{%v}] receive snapshot from {%v} ,LastSnapShotIncludeIndex ={%v} ", rf.me,
  //     args.LeaderId, args.LastSnapShotIncludeIndex)
  // 持久化
  m_persister->Save(persistData(), args->data());
}

void Raft::pushMsgToKvServer(ApplyMsg msg) { applyChan->Push(msg); }

void Raft::leaderHearBeatTicker() {
  while (true) {
    // broadcastTime应该比electionTimeout小一个数量级
    // 心跳一般设置 0.5 - 20ms，选举超时是 150 - 300ms
    while (m_status != Leader) {
      usleep(1000 * HeartBeatTimeout);
    }

    static std::atomic<int32_t> atomicCount = 0;

    std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};

    std::chrono::system_clock::time_point wakeTime{};
    {
      // 这里加锁是因为 m_lastResetHearBeatTime 会被共享
      std::lock_guard<std::mutex> lock(m_mtx);
      wakeTime = now();

      suitableSleepTime = std::chrono::milliseconds(HeartBeatTimeout) + m_lastResetHearBeatTime - wakeTime;
    }

    if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1) {
      std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数设置睡眠时间为: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                << std::endl;
      // 获取当前时间点
      auto start = std::chrono::steady_clock::now();

      usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
      // std::this_thread::sleep_for(suitableSleepTime);

      // 获取函数运行结束后的时间点
      auto end = std::chrono::steady_clock::now();

      // 计算时间差并输出结果（单位为毫秒）
      std::chrono::duration<double, std::milli> duration = end - start;

      // 使用ANSI控制序列将输出颜色修改为紫色
      std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数实际睡眠时间为: " << duration.count()
                << " 毫秒\033[0m" << std::endl;
      ++atomicCount;
    }

    if (std::chrono::duration<double, std::milli>(m_lastResetHearBeatTime - wakeTime).count() > 0) {
      continue;
    }

    doHeartBeat();
  }
}

void Raft::leaderSendSnapShot(int server) {
  m_mtx.lock();
  raftRpcProctoc::InstallSnapshotRequest args;
  args.set_leaderid(m_me);
  args.set_term(m_currentTerm);
  args.set_lastsnapshotincludeindex(m_lastSnapshotIncludeIndex);
  args.set_lastsnapshotincludeterm(m_lastSnapshotIncludeTerm);
  args.set_data(m_persister->ReadSnapshot());

  raftRpcProctoc::InstallSnapshotResponse reply;
  m_mtx.unlock();
  bool ok = m_peers[server]->InstallSnapshot(&args, &reply);
  m_mtx.lock();
  DEFER { m_mtx.unlock(); };
  if (!ok) {
    return;
  }
  if (m_status != Leader || m_currentTerm != args.term()) {
    return;  // 中间释放过锁，可能状态已经改变了
  }
  //	无论什么时候都要判断term
  if (reply.term() > m_currentTerm) {
    // 三变
    m_currentTerm = reply.term();
    m_votedFor = -1;
    m_status = Follower;
    persist();
    m_lastResetElectionTime = now();
    return;
  }
  m_matchIndex[server] = args.lastsnapshotincludeindex();
  m_nextIndex[server] = m_matchIndex[server] + 1;
}

void Raft::leaderUpdateCommitIndex() {
  m_commitIndex = m_lastSnapshotIncludeIndex;
  // for index := rf.commitIndex+1;index < len(rf.log);index++ {
  // for index := rf.getLastIndex();index>=rf.commitIndex+1;index--{
  for (int index = getLastLogIndex(); index >= m_lastSnapshotIncludeIndex + 1; index--) {
    int sum = 0;
    for (int i = 0; i < m_peers.size(); i++) {
      if (i == m_me) {
        sum += 1;
        continue;
      }
      if (m_matchIndex[i] >= index) {
        sum += 1;
      }
    }

    //        !!!只有当前term有新提交的，才会更新commitIndex！！！！
    // log.Printf("lastSSP:%d, index: %d, commitIndex: %d, lastIndex: %d",rf.lastSSPointIndex, index,
    // rf.commitIndex, rf.getLastIndex())
    if (sum >= m_peers.size() / 2 + 1 && getLogTermFromLogIndex(index) == m_currentTerm) {
      m_commitIndex = index;
      break;
    }
  }
  //    DPrintf("[func-leaderUpdateCommitIndex()-rf{%v}] Leader %d(term%d) commitIndex
  //    %d",rf.me,rf.me,rf.currentTerm,rf.commitIndex)
}

// 进来前要保证logIndex是存在的，即≥rf.lastSnapshotIncludeIndex	，而且小于等于rf.getLastLogIndex()
bool Raft::matchLog(int logIndex, int logTerm) {
  myAssert(logIndex >= m_lastSnapshotIncludeIndex && logIndex <= getLastLogIndex(),
           format("不满足：logIndex{%d}>=rf.lastSnapshotIncludeIndex{%d}&&logIndex{%d}<=rf.getLastLogIndex{%d}",
                  logIndex, m_lastSnapshotIncludeIndex, logIndex, getLastLogIndex()));
  return logTerm == getLogTermFromLogIndex(logIndex);
  // if logIndex == rf.lastSnapshotIncludeIndex {
  // 	return logTerm == rf.lastSnapshotIncludeTerm
  // } else {
  // 	return logTerm == rf.logs[rf.getSlicesIndexFromLogIndex(logIndex)].LogTerm
  // }
}

void Raft::persist() {
  // Your code here (2C).
  auto data = persistData();
  m_persister->SaveRaftState(data);
  // fmt.Printf("RaftNode[%d] persist starts, currentTerm[%d] voteFor[%d] log[%v]\n", rf.me, rf.currentTerm,
  // rf.votedFor, rf.logs) fmt.Printf("%v\n", string(data))
}

void Raft::RequestVote(const raftRpcProctoc::RequestVoteArgs* args, raftRpcProctoc::RequestVoteReply* reply) {
  std::lock_guard<std::mutex> lg(m_mtx);

  DEFER { persist(); };

  // 1. 请求节点的任期比我旧，直接拒绝
  if (args->term() < m_currentTerm) {
    reply->set_term(m_currentTerm);
    reply->set_votestate(Expire);  // 假设这是自定义枚举：已过期
    reply->set_votegranted(false);
    return;
  }

  // 2. 发现更大的任期，立刻降级为 Follower 并更新自己的任期
  if (args->term() > m_currentTerm) {
    m_status = Follower;
    m_currentTerm = args->term();
    m_votedFor = -1;  // 任期更新，重新清空投票记录
                      // 注意：这里绝对不能重置选举定时器，只有真正把票投出去才能重置
  }

  // 二次校验：此时双方的 term 逻辑上已经是对齐的了
  myAssert(args->term() == m_currentTerm, format("[func--rf{%d}] term equal assert failed", m_me));

  // 3. 检查 Candidate 的日志是否至少和自己一样新 (Raft 的选举限制)
  if (!UpToDate(args->lastlogindex(), args->lastlogterm())) {
    reply->set_term(m_currentTerm);
    reply->set_votestate(Voted);
    reply->set_votegranted(false);
    return;
  }

  // 4. 检查在本任期内，是否已经把票投给了别人
  // 注意：如果是同一个 candidateid 重复发来的拉票请求（网络重发），可以允许
  if (m_votedFor != -1 && m_votedFor != args->candidateid()) {
    reply->set_term(m_currentTerm);
    reply->set_votestate(Voted);
    reply->set_votegranted(false);
    return;
  }

  // 5. 满足所有条件，正式投票
  m_votedFor = args->candidateid();
  m_lastResetElectionTime = now();  // 【核心】只有在这里（确定投票后）才重置选举超时定时器

  reply->set_term(m_currentTerm);
  reply->set_votestate(Normal);
  reply->set_votegranted(true);
}

bool Raft::UpToDate(int index, int term) {
  // lastEntry := rf.log[len(rf.log)-1]

  int lastIndex = -1;
  int lastTerm = -1;
  getLastLogIndexAndTerm(&lastIndex, &lastTerm);
  return term > lastTerm || (term == lastTerm && index >= lastIndex);
}

void Raft::getLastLogIndexAndTerm(int* lastLogIndex, int* lastLogTerm) {
  // 这里两个分支是因为，系统存在一个快照机制，将长时间堆积的日志保存到文件
  // 当内存中的 m_logs 为空时，代表刚进行了一次 dump，此时只能从快照中去取
  if (m_logs.empty()) {
    *lastLogIndex = m_lastSnapshotIncludeIndex;
    *lastLogTerm = m_lastSnapshotIncludeTerm;
    return;
  } else {
    *lastLogIndex = m_logs[m_logs.size() - 1].logindex();
    *lastLogTerm = m_logs[m_logs.size() - 1].logterm();
    return;
  }
}
/**
 *
 * @return 最新的log的logindex，即log的逻辑index。区别于log在m_logs中的物理index
 * 可见：getLastLogIndexAndTerm()
 */
int Raft::getLastLogIndex() {
  int lastLogIndex = -1;
  int _ = -1;
  getLastLogIndexAndTerm(&lastLogIndex, &_);
  return lastLogIndex;
}

int Raft::getLastLogTerm() {
  int _ = -1;
  int lastLogTerm = -1;
  getLastLogIndexAndTerm(&_, &lastLogTerm);
  return lastLogTerm;
}

/**
 *
 * @param logIndex log的逻辑index。注意区别于m_logs的物理index
 * @return
 */
int Raft::getLogTermFromLogIndex(int logIndex) {
  myAssert(logIndex >= m_lastSnapshotIncludeIndex,
           format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} < rf.lastSnapshotIncludeIndex{%d}", m_me,
                  logIndex, m_lastSnapshotIncludeIndex));

  int lastLogIndex = getLastLogIndex();

  myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                            m_me, logIndex, lastLogIndex));

  if (logIndex == m_lastSnapshotIncludeIndex) {
    return m_lastSnapshotIncludeTerm;
  } else {
    return m_logs[getSlicesIndexFromLogIndex(logIndex)].logterm();
  }
}

int Raft::GetRaftStateSize() { return m_persister->RaftStateSize(); }

// 找到index对应的真实下标位置！！！
// 限制，输入的logIndex必须保存在当前的logs里面（不包含snapshot）
int Raft::getSlicesIndexFromLogIndex(int logIndex) {
  myAssert(logIndex > m_lastSnapshotIncludeIndex,
           format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} <= rf.lastSnapshotIncludeIndex{%d}", m_me,
                  logIndex, m_lastSnapshotIncludeIndex));
  int lastLogIndex = getLastLogIndex();
  myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                            m_me, logIndex, lastLogIndex));
  int SliceIndex = logIndex - m_lastSnapshotIncludeIndex - 1;
  return SliceIndex;
}

bool Raft::sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                           std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum) {
  // ... 前面的发送逻辑不变 ...
  bool ok = m_peers[server]->RequestVote(args.get(), reply.get());

  if (!ok) {
    return ok;  // 网络通信失败，必须退出，因为 reply 里的数据是脏的/未初始化的
  }

  std::lock_guard<std::mutex> lg(m_mtx);

  // 1. 检查任期是否变化 (RPC发送出去后，自己是不是又超时开启了新一轮选举)
  if (m_currentTerm != args->term()) {
    return true;  // 过期的回复，直接丢弃
  }

  // 2. 检查对方是不是有了更高的任期 (说明集群里有更新的Leader或Candidate)
  if (reply->term() > m_currentTerm) {
    m_status = Follower;
    m_currentTerm = reply->term();
    m_votedFor = -1;
    persist();
    return true;
  }

  // 3. 检查自己还是不是候选人 (关键防脑裂：可能在等RPC时已经变成了Follower)
  if (m_status != Candidate) {
    return true;
  }

  // 如果对方没投票，直接返回
  if (!reply->votegranted()) {
    return true;
  }

  // 票数+1
  *votedNum = *votedNum + 1;

  // 检查是否达到多数派
  if (*votedNum >= m_peers.size() / 2 + 1) {
    m_status = Leader;

    // 初始化 Leader 特有状态
    int lastLogIndex = getLastLogIndex();
    for (int i = 0; i < m_nextIndex.size(); i++) {
      m_nextIndex[i] = lastLogIndex + 1;
      m_matchIndex[i] = 0;
    }

    // todo: 建议改成通过协程调度器执行，而不是直接 std::thread
    std::thread t(&Raft::doHeartBeat, this);
    t.detach();

    persist();
  }
  return true;
}

bool Raft::sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                             std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,
                             std::shared_ptr<int> appendNums) {
  bool ok = m_peers[server]->AppendEntries(args.get(), reply.get());
  if (!ok || reply->appstate() == Disconnected) {
    return false;
  }

  std::lock_guard<std::mutex> lg(m_mtx);

  if (m_currentTerm != args->term()) {
    return true;
  }

  // leader 任期 更小
  if (reply->term() > m_currentTerm) {
    m_status = Follower;
    m_currentTerm = reply->term();
    m_votedFor = -1;
    persist();
    return true;
  }

  if (reply->term() < m_currentTerm) {
    return true;  // 忽略过期节点的回复
  }

  // 4. 自身身份校验 (可能在等锁的期间被降级为 Follower 了)
  if (m_status != Leader) {
    return true;
  }

  // 二次断言，此时任期必定相等
  myAssert(reply->term() == m_currentTerm, "reply.Term != m_currentTerm");

  // == -100 代表当前节点回退 fllower 了
  if (!reply->success()) {
    // 【处理日志冲突】
    if (reply->updatenextindex() != -100) {
      // 【优化 2：边界安全】确保 nextIndex 不会因为对端错误计算而变成 0 或负数
      m_nextIndex[server] = std::max(1, reply->updatenextindex());
      DPrintf("[func -sendAppendEntries rf{%d}] 返回不匹配，回缩nextIndex[%d]至: {%d}\n", m_me, server,
              m_nextIndex[server]);
    }
  } else {
    // 【处理日志追加成功】
    int matchIdx = args->prevlogindex() + args->entries_size();

    // 使用 std::max 防止乱序的旧 RPC 导致 matchIndex 倒退
    m_matchIndex[server] = std::max(m_matchIndex[server], matchIdx);
    m_nextIndex[server] = m_matchIndex[server] + 1;

    // 票数累加
    *appendNums += 1;

    // 检查是否达到多数派 (多数派 = 节点总数 / 2 + 1)
    if (*appendNums >= (m_peers.size() / 2) + 1) {
      *appendNums = 0;  // 置 0 保证幂等性，防止同一批次内多次触发

      // 【Raft 核心规则：只能提交当前任期的日志】
      // 论文 5.4.2 节：Leader 只能通过计算副本数来提交属于自己当前任期的日志
      if (args->entries_size() > 0 && args->entries(args->entries_size() - 1).logterm() == m_currentTerm) {
        if (matchIdx > m_commitIndex) {
          DPrintf("当前term有log成功提交，更新 m_commitIndex from{%d} to{%d}", m_commitIndex, matchIdx);
          m_commitIndex = matchIdx;

          m_applyCond.notify_one();
        }
      }
    }
  }

  return true;
}

void Raft::AppendEntries(google::protobuf::RpcController* controller,
                         const ::raftRpcProctoc::AppendEntriesArgs* request,
                         ::raftRpcProctoc::AppendEntriesReply* response, ::google::protobuf::Closure* done) {
  AppendEntries1(request, response);
  done->Run();
}

void Raft::InstallSnapshot(google::protobuf::RpcController* controller,
                           const ::raftRpcProctoc::InstallSnapshotRequest* request,
                           ::raftRpcProctoc::InstallSnapshotResponse* response, ::google::protobuf::Closure* done) {
  InstallSnapshot(request, response);

  done->Run();
}

void Raft::RequestVote(google::protobuf::RpcController* controller, const ::raftRpcProctoc::RequestVoteArgs* request,
                       ::raftRpcProctoc::RequestVoteReply* response, ::google::protobuf::Closure* done) {
  RequestVote(request, response);
  // 必须有 done->run()，这样 rpc 框架才能将response 发送回客户端
  done->Run();
}

void Raft::Start(Op command, int* newLogIndex, int* newLogTerm, bool* isLeader) {
  std::lock_guard<std::mutex> lg1(m_mtx);
  //    m_mtx.lock();
  //    Defer ec1([this]()->void {
  //       m_mtx.unlock();
  //    });
  if (m_status != Leader) {
    DPrintf("[func-Start-rf{%d}]  is not leader");
    *newLogIndex = -1;
    *newLogTerm = -1;
    *isLeader = false;
    return;
  }

  raftRpcProctoc::LogEntry newLogEntry;
  newLogEntry.set_command(command.asString());
  newLogEntry.set_logterm(m_currentTerm);
  newLogEntry.set_logindex(getNewCommandIndex());
  m_logs.emplace_back(newLogEntry);

  int lastLogIndex = getLastLogIndex();

  // leader应该不停的向各个Follower发送AE来维护心跳和保持日志同步，目前的做法是新的命令来了不会直接执行，而是等待leader的心跳触发
  DPrintf("[func-Start-rf{%d}]  lastLogIndex:%d,command:%s\n", m_me, lastLogIndex, &command);
  // rf.timer.Reset(10) //接收到命令后马上给follower发送,改成这样不知为何会出现问题，待修正 todo
  persist();
  *newLogIndex = newLogEntry.logindex();
  *newLogTerm = newLogEntry.logterm();
  *isLeader = true;
}

// Make
// the service or tester wants to create a Raft server. the ports
// of all the Raft servers (including this one) are in peers[]. this
// server's port is peers[me]. all the servers' peers[] arrays
// have the same order. persister is a place for this server to
// save its persistent state, and also initially holds the most
// recent saved state, if any. applyCh is a channel on which the
// tester or service expects Raft to send ApplyMsg messages.
// Make() must return quickly, so it should start goroutines
// for any long-running work.
void Raft::init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh) {
  m_peers = peers;
  m_persister = persister;
  m_me = me;
  // Your initialization code here (2A, 2B, 2C).
  m_mtx.lock();

  // applier
  this->applyChan = applyCh;
  //    rf.ApplyMsgQueue = make(chan ApplyMsg)
  m_currentTerm = 0;
  m_status = Follower;
  m_commitIndex = 0;
  m_lastApplied = 0;
  m_logs.clear();
  for (int i = 0; i < m_peers.size(); i++) {
    m_matchIndex.push_back(0);
    m_nextIndex.push_back(0);
  }
  m_votedFor = -1;

  m_lastSnapshotIncludeIndex = 0;
  m_lastSnapshotIncludeTerm = 0;
  m_lastResetElectionTime = now();
  m_lastResetHearBeatTime = now();

  // initialize from state persisted before a crash
  readPersist(m_persister->ReadRaftState());
  if (m_lastSnapshotIncludeIndex > 0) {
    m_lastApplied = m_lastSnapshotIncludeIndex;
    // rf.commitIndex = rf.lastSnapshotIncludeIndex   todo ：崩溃恢复为何不能读取commitIndex
  }

  DPrintf("[Init&ReInit] Sever %d, term %d, lastSnapshotIncludeIndex {%d} , lastSnapshotIncludeTerm {%d}", m_me,
          m_currentTerm, m_lastSnapshotIncludeIndex, m_lastSnapshotIncludeTerm);

  m_mtx.unlock();

  // 创建协程对象
  m_ioManager = std::make_unique<monsoon::IOManager>(FIBER_THREAD_NUM, FIBER_USE_CALLER_THREAD);

  m_ioManager->scheduler([this]() -> void { this->leaderHearBeatTicker(); });
  m_ioManager->scheduler([this]() -> void { this->electionTimeOutTicker(); });

  std::thread t3(&Raft::applierTicker, this);
  t3.detach();
}

std::string Raft::persistData() {
  BoostPersistRaftNode boostPersistRaftNode;
  boostPersistRaftNode.m_currentTerm = m_currentTerm;
  boostPersistRaftNode.m_votedFor = m_votedFor;
  boostPersistRaftNode.m_lastSnapshotIncludeIndex = m_lastSnapshotIncludeIndex;
  boostPersistRaftNode.m_lastSnapshotIncludeTerm = m_lastSnapshotIncludeTerm;
  for (auto& item : m_logs) {
    boostPersistRaftNode.m_logs.push_back(item.SerializeAsString());
  }

  std::stringstream ss;
  boost::archive::text_oarchive oa(ss);
  oa << boostPersistRaftNode;
  return ss.str();
}

void Raft::readPersist(std::string data) {
  if (data.empty()) {
    return;
  }
  // 这里将data反序列化，使用 boost 实现
  std::stringstream iss(data);
  boost::archive::text_iarchive ia(iss);
  // read class state from archive
  BoostPersistRaftNode boostPersistRaftNode;
  ia >> boostPersistRaftNode;

  m_currentTerm = boostPersistRaftNode.m_currentTerm;
  m_votedFor = boostPersistRaftNode.m_votedFor;
  m_lastSnapshotIncludeIndex = boostPersistRaftNode.m_lastSnapshotIncludeIndex;
  m_lastSnapshotIncludeTerm = boostPersistRaftNode.m_lastSnapshotIncludeTerm;
  m_logs.clear();
  for (auto& item : boostPersistRaftNode.m_logs) {
    raftRpcProctoc::LogEntry logEntry;
    logEntry.ParseFromString(item);
    m_logs.emplace_back(logEntry);
  }
}

void Raft::Snapshot(int index, std::string snapshot) {
  std::lock_guard<std::mutex> lg(m_mtx);

  if (m_lastSnapshotIncludeIndex >= index || index > m_commitIndex) {
    DPrintf(
        "[func-Snapshot-rf{%d}] rejects replacing log with snapshotIndex %d as current snapshotIndex %d is larger "
        "or "
        "smaller ",
        m_me, index, m_lastSnapshotIncludeIndex);
    return;
  }
  auto lastLogIndex = getLastLogIndex();  // 为了检查snapshot前后日志是否一样，防止多截取或者少截取日志

  // 制造完此快照后剩余的所有日志
  int newLastSnapshotIncludeIndex = index;
  int newLastSnapshotIncludeTerm = m_logs[getSlicesIndexFromLogIndex(index)].logterm();
  std::vector<raftRpcProctoc::LogEntry> trunckedLogs;
  // todo :这种写法有点笨，待改进，而且有内存泄漏的风险
  for (int i = index + 1; i <= getLastLogIndex(); i++) {
    // 注意有=，因为要拿到最后一个日志
    trunckedLogs.push_back(m_logs[getSlicesIndexFromLogIndex(i)]);
  }
  m_lastSnapshotIncludeIndex = newLastSnapshotIncludeIndex;
  m_lastSnapshotIncludeTerm = newLastSnapshotIncludeTerm;
  m_logs = trunckedLogs;
  m_commitIndex = std::max(m_commitIndex, index);
  m_lastApplied = std::max(m_lastApplied, index);

  // rf.lastApplied = index //lastApplied 和 commit应不应该改变呢？？？ 为什么  不应该改变吧
  m_persister->Save(persistData(), snapshot);

  DPrintf("[SnapShot]Server %d snapshot snapshot index {%d}, term {%d}, loglen {%d}", m_me, index,
          m_lastSnapshotIncludeTerm, m_logs.size());
  myAssert(m_logs.size() + m_lastSnapshotIncludeIndex == lastLogIndex,
           format("len(rf.logs){%d} + rf.lastSnapshotIncludeIndex{%d} != lastLogjInde{%d}", m_logs.size(),
                  m_lastSnapshotIncludeIndex, lastLogIndex));
}