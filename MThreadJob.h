// MThreadJob.h --- スレッド ジョブ。
// Author: katahiromz
// License: MIT
#pragma once
#include <windows.h>
#include <process.h>
#include <functional>
#include <tuple>
#include <utility>
#include <type_traits>

template <typename T_RESULT = HRESULT>
class MThreadJob {
public:
	//------------------------------------------------------------------
	// 排他ロックのRAIIラッパー(Win32 Mutex は同一スレッドで再入可能)
	//------------------------------------------------------------------
	class ScopedLock {
	public:
		explicit ScopedLock(const MThreadJob& job) : m_job(job), m_locked(job.Lock()) {}
		~ScopedLock() { if (m_locked) m_job.Unlock(); }
		ScopedLock(const ScopedLock&) = delete;
		ScopedLock& operator=(const ScopedLock&) = delete;
		explicit operator bool() const { return m_locked; }
	private:
		const MThreadJob& m_job;
		BOOL m_locked;
	};

	//------------------------------------------------------------------
	// ワーカースレッド内から使うための窓口
	//------------------------------------------------------------------
	class Context {
	public:
		explicit Context(MThreadJob& job) : m_job(job) {}

		// 停止イベントのハンドル(WaitForMultipleObjects等に直接渡したい場合)
		HANDLE StopEvent() const { return m_job.m_hStopEvent; }

		// 停止が要求されているか?
		BOOL IsStopRequested() const {
			return ::WaitForSingleObject(m_job.m_hStopEvent, 0) == WAIT_OBJECT_0;
		}

		// 最大 dwMilliseconds 待つ。停止が要求されたら即座に TRUE を返す。
		BOOL WaitStop(DWORD dwMilliseconds) const {
			return ::WaitForSingleObject(m_job.m_hStopEvent, dwMilliseconds) == WAIT_OBJECT_0;
		}

		// ハンドルと停止イベントを同時に待つ。
		//   WAIT_OBJECT_0     : h がシグナルされた
		//   WAIT_OBJECT_0 + 1 : 停止が要求された
		//   WAIT_TIMEOUT      : タイムアウト
		DWORD WaitWithStop(HANDLE h, DWORD dwMilliseconds = INFINITE) const {
			HANDLE handles[2] = { h, m_job.m_hStopEvent };
			return ::WaitForMultipleObjects(2, handles, FALSE, dwMilliseconds);
		}

		// 結果(進捗など)の設定/取得。外部の GetResult から読める。
		HRESULT SetResult(const T_RESULT& ret) { return m_job._SetResult(ret); }
		HRESULT GetResult(T_RESULT& ret) const { return m_job.GetResult(ret); }

		// スコープロック: auto lock = ctx.Lock();
		ScopedLock Lock() const { return ScopedLock(m_job); }

	private:
		MThreadJob& m_job;
	};

	//------------------------------------------------------------------
	// 構築・破棄
	//------------------------------------------------------------------

	// hMutex / hStopEvent の所有権は MThreadJob に移る(デストラクタで閉じる)。
	// nullptr を渡した場合は内部で作成する。
	MThreadJob(HANDLE hMutex = nullptr, HANDLE hStopEvent = nullptr) {
		m_hMutex = hMutex ? hMutex : ::CreateMutexW(nullptr, FALSE, nullptr);
		m_hStopEvent = hStopEvent ? hStopEvent : ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}

	~MThreadJob() {
		if (m_hThread) {
			Stop();
			::CloseHandle(m_hThread);
		}
		if (m_hMutex) ::CloseHandle(m_hMutex);
		if (m_hStopEvent) ::CloseHandle(m_hStopEvent);
	}

	MThreadJob(const MThreadJob&) = delete;
	MThreadJob& operator=(const MThreadJob&) = delete;

	//------------------------------------------------------------------
	// 起動
	//------------------------------------------------------------------
	// fnThread : void/任意型 fn(Context& ctx, args...)
	//            T_RESULT に変換できる値を return すると、それが結果になる。
	// fnReturn : 指定すると fnThread 終了後に呼ばれ、その値で上書きされる(nullptr可)。
	// 参照で渡したい引数は std::ref(x) を使うこと。
	template <typename F, typename... Args>
	BOOL Startup(std::function<T_RESULT()> fnReturn, F&& fnThread, Args&&... args) {
		return _StartThread(std::move(fnReturn),
			_Bind(std::forward<F>(fnThread), std::forward<Args>(args)...));
	}

	// fnReturn 不要版。
	template <typename F, typename... Args>
	BOOL Run(F&& fnThread, Args&&... args) {
		return Startup(nullptr, std::forward<F>(fnThread), std::forward<Args>(args)...);
	}

	//------------------------------------------------------------------
	// 制御
	//------------------------------------------------------------------

	// 停止イベントを立て、スレッドの終了を待つ。
	// ※ワーカースレッド自身から呼ぶとデッドロックするので注意。
	BOOL Stop() {
		::SetEvent(m_hStopEvent);
		return Wait(INFINITE) == WAIT_OBJECT_0;
	}

	// スレッドの終了を待つ。スレッドが無ければ即 WAIT_OBJECT_0。
	DWORD Wait(DWORD dwMilliseconds = INFINITE) {
		if (!m_hThread)
			return WAIT_OBJECT_0;
		return ::WaitForSingleObject(m_hThread, dwMilliseconds);
	}

	// 実行中？
	BOOL IsRunning() const {
		return m_hThread && ::WaitForSingleObject(m_hThread, 0) == WAIT_TIMEOUT;
	}

	// 結果を取得。
	HRESULT GetResult(T_RESULT& ret) const {
		if (Lock()) {
			ret = m_ret;
			Unlock();
			return S_OK;
		}
		return E_FAIL;
	}

	BOOL Lock() const {
		return ::WaitForSingleObject(m_hMutex, INFINITE) == WAIT_OBJECT_0;
	}
	void Unlock() const {
		::ReleaseMutex(m_hMutex);
	}

protected:
	HANDLE m_hMutex = nullptr;
	HANDLE m_hStopEvent = nullptr;
	HANDLE m_hThread = nullptr;
	std::function<void(Context&)> m_fnThread = nullptr;
	std::function<T_RESULT()> m_fnReturn = nullptr;
	T_RESULT m_ret = {};

	HRESULT _SetResult(const T_RESULT& ret) {
		if (!Lock())
			return E_FAIL;
		m_ret = ret;
		Unlock();
		return S_OK;
	}

	// ユーザー関数 + 引数を std::function<void(Context&)> にまとめる。
	template <typename F, typename... Args>
	static std::function<void(Context&)> _Bind(F&& f, Args&&... args) {
		using Fn = std::decay_t<F>;
		static_assert(std::is_invocable_v<Fn&, Context&, std::decay_t<Args>&...>,
			"fnThread must be callable as fn(MThreadJob::Context&, args...)");
		return [fn = Fn(std::forward<F>(f)),
		        tup = std::make_tuple(std::forward<Args>(args)...)](Context& ctx) mutable
		{
			std::apply([&](auto&... a) {
				using R = std::invoke_result_t<Fn&, Context&, decltype(a)...>;
				if constexpr (std::is_void_v<R>) {
					fn(ctx, a...);
				} else {
					R r = fn(ctx, a...);
					if constexpr (std::is_convertible_v<R, T_RESULT>)
						ctx.SetResult(static_cast<T_RESULT>(r));
				}
			}, tup);
		};
	}

	BOOL _StartThread(std::function<T_RESULT()> fnReturn, std::function<void(Context&)> fnThread) {
		if (!m_hMutex || !m_hStopEvent)
			return FALSE;
		if (IsRunning())
			return FALSE; // 既に実行中

		if (m_hThread) {
			::CloseHandle(m_hThread);
			m_hThread = nullptr;
		}

		m_fnReturn = std::move(fnReturn);
		m_fnThread = std::move(fnThread);

		_SetResult(T_RESULT{});
		::ResetEvent(m_hStopEvent);

		unsigned threadId = 0;
		uintptr_t h = ::_beginthreadex(nullptr, 0, &MThreadJob::_ThreadProc, this, 0, &threadId);
		if (h == 0) {
			m_fnThread = nullptr;
			m_fnReturn = nullptr;
			return FALSE;
		}
		m_hThread = reinterpret_cast<HANDLE>(h);
		return TRUE;
	}

	static unsigned __stdcall _ThreadProc(void* pv) {
		auto* self = static_cast<MThreadJob*>(pv);
		try {
			Context ctx(*self);
			if (self->m_fnThread)
				self->m_fnThread(ctx);
			if (self->m_fnReturn)
				self->_SetResult(self->m_fnReturn());
		} catch (...) {
			// ワーカースレッドから例外を漏らさない
		}
		return 0;
	}
};
