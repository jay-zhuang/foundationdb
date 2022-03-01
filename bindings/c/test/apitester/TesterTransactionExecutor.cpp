/*
 * TesterTransactionExecutor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "TesterTransactionExecutor.h"
#include "TesterUtil.h"
#include "test/apitester/TesterScheduler.h"
#include <iostream>
#include <memory>
#include <unordered_map>

namespace FdbApiTester {

namespace {

void fdb_check(fdb_error_t e) {
	if (e) {
		std::cerr << fdb_get_error(e) << std::endl;
		std::abort();
	}
}

} // namespace

void ITransactionContext::continueAfterAll(std::shared_ptr<std::vector<Future>> futures, TTaskFct cont) {
	auto counter = std::make_shared<std::atomic<int>>(futures->size());
	for (auto& f : *futures) {
		continueAfter(f, [counter, cont]() {
			if (--(*counter) == 0) {
				cont();
			}
		});
	}
}

class TransactionContext : public ITransactionContext {
public:
	TransactionContext(FDBTransaction* tx,
	                   std::shared_ptr<ITransactionActor> txActor,
	                   TTaskFct cont,
	                   const TransactionExecutorOptions& options,
	                   IScheduler* scheduler)
	  : options(options), fdbTx(tx), txActor(txActor), contAfterDone(cont), scheduler(scheduler), finalError(0) {}

	Transaction* tx() override { return &fdbTx; }
	void continueAfter(Future f, TTaskFct cont) override { doContinueAfter(f, cont); }
	void commit() override {
		Future f = fdbTx.commit();
		doContinueAfter(f, [this]() { done(); });
	}
	void done() override {
		TTaskFct cont = contAfterDone;
		ASSERT(!onErrorFuture);
		ASSERT(waitMap.empty());
		delete this;
		cont();
	}

private:
	void doContinueAfter(Future f, TTaskFct cont) {
		if (options.blockOnFutures) {
			blockingContinueAfter(f, cont);
		} else {
			asyncContinueAfter(f, cont);
		}
	}

	void blockingContinueAfter(Future f, TTaskFct cont) {
		scheduler->schedule([this, f, cont]() mutable {
			std::unique_lock<std::mutex> lock(mutex);
			if (!onErrorFuture) {
				fdb_check(fdb_future_block_until_ready(f.fdbFuture()));
				fdb_error_t err = f.getError();
				if (err) {
					if (err != error_code_transaction_cancelled) {
						onErrorFuture = fdbTx.onError(err);
						fdb_check(fdb_future_block_until_ready(onErrorFuture.fdbFuture()));
						scheduler->schedule([this]() { handleOnErrorResult(); });
					}
				} else {
					scheduler->schedule([cont]() { cont(); });
				}
			}
		});
	}

	void asyncContinueAfter(Future f, TTaskFct cont) {
		std::unique_lock<std::mutex> lock(mutex);
		if (!onErrorFuture) {
			waitMap[f.fdbFuture()] = WaitInfo{ f, cont };
			lock.unlock();
			fdb_check(fdb_future_set_callback(f.fdbFuture(), futureReadyCallback, this));
		}
	}

	static void futureReadyCallback(FDBFuture* f, void* param) {
		TransactionContext* txCtx = (TransactionContext*)param;
		txCtx->onFutureReady(f);
	}

	void onFutureReady(FDBFuture* f) {
		std::unique_lock<std::mutex> lock(mutex);
		auto iter = waitMap.find(f);
		if (iter == waitMap.end()) {
			return;
		}
		fdb_error_t err = fdb_future_get_error(f);
		TTaskFct cont = iter->second.cont;
		waitMap.erase(iter);
		if (err) {
			if (err != error_code_transaction_cancelled) {
				waitMap.clear();
				onErrorFuture = tx()->onError(err);
				lock.unlock();
				fdb_check(fdb_future_set_callback(onErrorFuture.fdbFuture(), onErrorReadyCallback, this));
			}
		} else {
			scheduler->schedule(cont);
		}
	}

	static void onErrorReadyCallback(FDBFuture* f, void* param) {
		TransactionContext* txCtx = (TransactionContext*)param;
		txCtx->onErrorReady(f);
	}

	void onErrorReady(FDBFuture* f) {
		scheduler->schedule([this]() { handleOnErrorResult(); });
	}

	void handleOnErrorResult() {
		std::unique_lock<std::mutex> lock(mutex);
		fdb_error_t err = onErrorFuture.getError();
		onErrorFuture.reset();
		if (err) {
			finalError = err;
			std::cout << "Fatal error: " << fdb_get_error(finalError) << std::endl;
			ASSERT(false);
			done();
		} else {
			lock.unlock();
			txActor->reset();
			txActor->start();
		}
	}

	struct WaitInfo {
		Future future;
		TTaskFct cont;
	};

	const TransactionExecutorOptions& options;
	Transaction fdbTx;
	std::shared_ptr<ITransactionActor> txActor;
	std::mutex mutex;
	std::unordered_map<FDBFuture*, WaitInfo> waitMap;
	Future onErrorFuture;
	TTaskFct contAfterDone;
	IScheduler* scheduler;
	fdb_error_t finalError;
};

class TransactionExecutor : public ITransactionExecutor {
public:
	TransactionExecutor() : scheduler(nullptr) {}

	~TransactionExecutor() { release(); }

	void init(IScheduler* scheduler, const char* clusterFile, const TransactionExecutorOptions& options) override {
		this->scheduler = scheduler;
		this->options = options;
		for (int i = 0; i < options.numDatabases; i++) {
			FDBDatabase* db;
			fdb_check(fdb_create_database(clusterFile, &db));
			databases.push_back(db);
		}
	}

	void execute(std::shared_ptr<ITransactionActor> txActor, TTaskFct cont) override {
		int idx = random.randomInt(0, options.numDatabases - 1);
		FDBTransaction* tx;
		fdb_check(fdb_database_create_transaction(databases[idx], &tx));
		TransactionContext* ctx = new TransactionContext(tx, txActor, cont, options, scheduler);
		txActor->init(ctx);
		txActor->start();
	}

	void release() override {
		for (FDBDatabase* db : databases) {
			fdb_database_destroy(db);
		}
	}

private:
	std::vector<FDBDatabase*> databases;
	TransactionExecutorOptions options;
	IScheduler* scheduler;
	Random random;
};

std::unique_ptr<ITransactionExecutor> createTransactionExecutor() {
	return std::make_unique<TransactionExecutor>();
}

} // namespace FdbApiTester