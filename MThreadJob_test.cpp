// MThreadJob_test.cpp --- スレッド ジョブの単体テスト。
// Author: katahiromz
// License: MIT

#include "MThreadJob.h"
#include <atomic>
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(expr) do { \
	if (!(expr)) { ::printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++g_fail; } \
} while (0)
#define RUN(fn) do { ::printf("[ RUN  ] %s\n", #fn); fn(); ::printf("[ DONE ] %s\n", #fn); } while (0)

using JobInt = MThreadJob<int>;
using JobHr  = MThreadJob<HRESULT>;
using Ctx    = JobInt::Context;

// 1. 正常終了し、fnReturn の値を取得できる
static void Test_NormalCompletion() {
	std::atomic<int> counter{0};
	JobHr job;
	CHECK(job.Startup([] { return S_OK; }, [&](JobHr::Context&) { counter = 42; }));
	CHECK(job.Wait(5000) == WAIT_OBJECT_0);
	CHECK(counter == 42);
	HRESULT hr = E_FAIL;
	CHECK(job.GetResult(hr) == S_OK);
	CHECK(hr == S_OK);
	CHECK(!job.IsRunning());
}

// 2. IsStopRequested / WaitStop でループを抜けられる
static void Test_StopLoop() {
	std::atomic<int> loops{0};
	JobInt job;
	CHECK(job.Run([&](Ctx& ctx) {
		while (!ctx.IsStopRequested()) {
			++loops;
			if (ctx.WaitStop(5)) break;
		}
	}));
	::Sleep(50);
	CHECK(job.IsRunning());
	CHECK(job.Stop());
	CHECK(loops > 0);
	CHECK(!job.IsRunning());
}

// 3. Stop 後に fnReturn の値が取れる
static void Test_StopWithFnReturn() {
	JobHr job;
	CHECK(job.Startup([] { return S_FALSE; },
		[](JobHr::Context& ctx) { while (!ctx.WaitStop(10)) {} }));
	::Sleep(30);
	CHECK(job.Stop());
	HRESULT hr = E_FAIL;
	CHECK(job.GetResult(hr) == S_OK);
	CHECK(hr == S_FALSE);
}

// 4. WaitStop は停止要求で即座に返る
static void Test_WaitStopIsInterruptible() {
	JobInt job;
	CHECK(job.Run([](Ctx& ctx) { ctx.WaitStop(60000); }));
	::Sleep(20);
	DWORD t0 = ::GetTickCount();
	CHECK(job.Stop());
	CHECK(::GetTickCount() - t0 < 5000);
}

// 5. fnThread の戻り値がそのまま GetResult で取れる
static void Test_ReturnValue() {
	JobInt job;
	CHECK(job.Run([](Ctx&) { return 123; }));
	CHECK(job.Wait(5000) == WAIT_OBJECT_0);
	int ret = 0;
	CHECK(job.GetResult(ret) == S_OK);
	CHECK(ret == 123);
}

// 6. 実行中に SetResult した進捗を外部から読める
static void Test_Progress() {
	JobInt job;
	CHECK(job.Run([](Ctx& ctx) {
		for (int i = 1; i <= 100; ++i) {
			ctx.SetResult(i);
			if (ctx.WaitStop(10)) return;
		}
	}));
	int ret = 0;
	for (int i = 0; i < 200 && ret < 3; ++i) {
		::Sleep(10);
		CHECK(job.GetResult(ret) == S_OK);
	}
	CHECK(ret >= 3);
	CHECK(job.Stop());
	int fin = 0;
	CHECK(job.GetResult(fin) == S_OK);
	CHECK(fin >= ret);
}

// 7. 引数(値コピー・std::ref)を渡せる
static void Test_Args() {
	int viaRef = 0;
	JobInt job;
	CHECK(job.Run(
		[](Ctx&, int n, std::string s, int& r) {
			r = n + (int)s.size();
			return r;
		},
		10, std::string("abc"), std::ref(viaRef)));
	CHECK(job.Wait(5000) == WAIT_OBJECT_0);
	CHECK(viaRef == 13);
	int ret = 0;
	CHECK(job.GetResult(ret) == S_OK);
	CHECK(ret == 13);
}

// 8. fnReturn は fnThread の return 値を上書きする
static void Test_FnReturnOverrides() {
	JobInt job;
	CHECK(job.Startup([] { return 2; }, [](Ctx&) { return 1; }));
	CHECK(job.Wait(5000) == WAIT_OBJECT_0);
	int ret = 0;
	CHECK(job.GetResult(ret) == S_OK);
	CHECK(ret == 2);
}

// 9. WaitWithStop: ハンドルのシグナル / 停止 / タイムアウトを区別できる
static void Test_WaitWithStop() {
	HANDLE hEvt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
	std::atomic<DWORD> r1{0}, r2{0}, r3{0};
	JobInt job;
	CHECK(job.Run([&](Ctx& ctx) {
		r1 = ctx.WaitWithStop(hEvt, 10);       // タイムアウト
		r2 = ctx.WaitWithStop(hEvt, 5000);     // 外部が hEvt をセット
		r3 = ctx.WaitWithStop(hEvt, INFINITE); // 外部が Stop
	}));
	::Sleep(100);
	::SetEvent(hEvt);
	::Sleep(100);
	CHECK(job.Stop());
	CHECK(r1 == WAIT_TIMEOUT);
	CHECK(r2 == WAIT_OBJECT_0);
	CHECK(r3 == WAIT_OBJECT_0 + 1);
	::CloseHandle(hEvt);
}

// 10. ScopedLock: スコープを抜けると解放される(再入も可能)
static void Test_ScopedLock() {
	JobInt job;
	{
		JobInt::ScopedLock lock(job);
		CHECK(lock);
	}
	CHECK(job.Run([](Ctx& ctx) {
		auto lock = ctx.Lock();
		if (!lock) return;
		ctx.SetResult(5); // 同一スレッドでの再入
	}));
	CHECK(job.Wait(5000) == WAIT_OBJECT_0);
	int ret = 0;
	CHECK(job.GetResult(ret) == S_OK);
	CHECK(ret == 5);
}

// 11. ロック保持中は外部の GetResult がブロックされる
static void Test_ScopedLock_Blocks() {
	std::atomic<bool> locked{false};
	JobInt job;
	CHECK(job.Run([&](Ctx& ctx) {
		auto lock = ctx.Lock();
		locked = true;
		ctx.WaitStop(200);
	}));
	while (!locked) ::Sleep(1);
	DWORD t0 = ::GetTickCount();
	int ret = 0;
	CHECK(job.GetResult(ret) == S_OK);
	CHECK(::GetTickCount() - t0 >= 100);
	CHECK(job.Stop());
}

// 12. 変換不能な結果型は無視される
static void Test_IgnoredReturnType() {
	JobHr job;
	CHECK(job.Run([](JobHr::Context&) { return std::string("x"); }));
	CHECK(job.Wait(5000) == WAIT_OBJECT_0);
	HRESULT hr = E_FAIL;
	CHECK(job.GetResult(hr) == S_OK);
	CHECK(hr == S_OK); // T_RESULT{} のまま
}

// 13. 実行中の二重 Startup は失敗する
static void Test_DoubleStartup() {
	MThreadJob<> job;
	auto loop = [](MThreadJob<>::Context& ctx) { ctx.WaitStop(INFINITE); };
	CHECK(job.Run(loop));
	CHECK(!job.Run(loop));
	CHECK(job.Stop());
}

// 14. 終了後に再 Startup できる
static void Test_Restart() {
	std::atomic<int> count{0};
	JobInt job;
	for (int i = 1; i <= 3; ++i) {
		CHECK(job.Startup([i] { return i; }, [&](Ctx&) { ++count; }));
		CHECK(job.Wait(5000) == WAIT_OBJECT_0);
		int ret = 0;
		CHECK(job.GetResult(ret) == S_OK);
		CHECK(ret == i);
	}
	CHECK(count == 3);
}

// 15. Wait のタイムアウト
static void Test_WaitTimeout() {
	MThreadJob<> job;
	CHECK(job.Run([](MThreadJob<>::Context& ctx) { ctx.WaitStop(INFINITE); }));
	CHECK(job.Wait(50) == WAIT_TIMEOUT);
	CHECK(job.IsRunning());
	CHECK(job.Stop());
}

// 16. デストラクタが実行中スレッドを停止する
static void Test_DestructorStops() {
	std::atomic<bool> exited{false};
	{
		MThreadJob<> job;
		CHECK(job.Run([&](MThreadJob<>::Context& ctx) {
			ctx.WaitStop(INFINITE);
			exited = true;
		}));
		::Sleep(20);
	}
	CHECK(exited);
}

// 17. スレッド内の例外でプロセスが落ちない
static void Test_Exception() {
	JobHr job;
	CHECK(job.Startup([] { return S_OK; }, [](JobHr::Context&) { throw 1; }));
	CHECK(job.Wait(5000) == WAIT_OBJECT_0);
	HRESULT hr = E_FAIL;
	CHECK(job.GetResult(hr) == S_OK);
	CHECK(hr == S_OK); // fnReturn は呼ばれず T_RESULT{} のまま
}

// 18. 外部から渡したハンドルを使える(所有権は MThreadJob へ)
static void Test_ExternalHandles() {
	HANDLE hMutex = ::CreateMutexW(nullptr, FALSE, nullptr);
	HANDLE hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	MThreadJob<> job(hMutex, hEvent);
	CHECK(job.Run([](MThreadJob<>::Context& ctx) { ctx.WaitStop(INFINITE); }));
	CHECK(job.Stop());
	CHECK(::WaitForSingleObject(hEvent, 0) == WAIT_OBJECT_0);
}

int main() {
	RUN(Test_NormalCompletion);
	RUN(Test_StopLoop);
	RUN(Test_StopWithFnReturn);
	RUN(Test_WaitStopIsInterruptible);
	RUN(Test_ReturnValue);
	RUN(Test_Progress);
	RUN(Test_Args);
	RUN(Test_FnReturnOverrides);
	RUN(Test_WaitWithStop);
	RUN(Test_ScopedLock);
	RUN(Test_ScopedLock_Blocks);
	RUN(Test_IgnoredReturnType);
	RUN(Test_DoubleStartup);
	RUN(Test_Restart);
	RUN(Test_WaitTimeout);
	RUN(Test_DestructorStops);
	RUN(Test_Exception);
	RUN(Test_ExternalHandles);

	::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nAll tests passed\n", g_fail);
	return g_fail ? 1 : 0;
}
