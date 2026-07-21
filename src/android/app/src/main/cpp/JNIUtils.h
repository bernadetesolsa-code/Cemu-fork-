#pragma once

#include <jni.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <exception>

namespace JNIUtils
{
	void SetJavaVM(JavaVM* jvm);

	inline std::string FromJString(JNIEnv* env, jstring jstr)
	{
		if (jstr == nullptr)
			return {};
		const char* c_str = env->GetStringUTFChars(jstr, nullptr);
		std::string str(c_str);
		env->ReleaseStringUTFChars(jstr, c_str);
		return str;
	}

	inline jstring ToJString(JNIEnv* env, const std::string& str)
	{
		return env->NewStringUTF(str.c_str());
	}

	inline jstring ToJString(JNIEnv* env, std::string_view str)
	{
		return ToJString(env, std::string(str));
	}

	inline jstring ToJString(JNIEnv* env, std::wstring_view str)
	{
		return ToJString(env, boost::nowide::narrow(str));
	}

	inline void HandleNativeException(JNIEnv* env, std::invocable auto fn)
	{
		try
		{
			fn();
		} catch (const std::exception& exception)
		{
			jclass exceptionClass = env->FindClass("info/cemu/cemu/nativeinterface/NativeException");
			env->ThrowNew(exceptionClass, exception.what());
		} catch (...)
		{
			jclass exceptionClass = env->FindClass("info/cemu/cemu/nativeinterface/NativeException");
			env->ThrowNew(exceptionClass, "Unknown native exception");
		}
	}

	JNIEnv* GetEnv();

	class Scopedjobject
	{
	  public:
		Scopedjobject() = default;

		Scopedjobject(Scopedjobject&& other) noexcept;

		void DeleteReference();

		Scopedjobject& operator=(Scopedjobject&& other) noexcept;

		jobject operator*() const;

		explicit Scopedjobject(jobject obj);

		~Scopedjobject();

	  private:
		jobject m_jobject = nullptr;
	};

	class Scopedjclass
	{
	  public:
		Scopedjclass() = default;

		Scopedjclass(Scopedjclass&& other) noexcept;

		explicit Scopedjclass(jclass javaClass);

		Scopedjclass& operator=(Scopedjclass&& other) noexcept;

		explicit Scopedjclass(const char* className);

		~Scopedjclass();

		jclass operator*() const;

	  private:
		jclass m_jclass = nullptr;
	};

	Scopedjobject GetEnumValue(JNIEnv* env, const std::string& enumClassName, const std::string& enumName);

	template<std::ranges::sized_range Range>
	jlongArray CreateLongArray(JNIEnv* env, Range&& range)
		requires std::convertible_to<std::ranges::range_value_t<Range>, jlong>
	{
		auto size = std::ranges::size(range);
		jlongArray array = env->NewLongArray(static_cast<jsize>(size));

		std::vector<jlong> buffer;
		buffer.reserve(size);

		for (auto v : range)
		{
			buffer.push_back(static_cast<jlong>(v));
		}

		env->SetLongArrayRegion(array, 0, static_cast<jsize>(size), buffer.data());

		return array;
	}

	template<std::ranges::input_range Range>
		requires(!std::ranges::sized_range<Range>)
	jlongArray CreateLongArray(JNIEnv* env, Range&& range)
	{
		std::vector<std::ranges::range_value_t<Range>> vector;

		for (auto&& e : range)
		{
			vector.push_back(static_cast<decltype(e)&&>(e));
		}

		return CreateLongArray(env, vector);
	}

	template<typename F, typename Elem, typename Result>
	concept JNITransform = requires(F f, Elem e) {{ f(e) } -> std::convertible_to<Result>; };

	template<std::ranges::sized_range Range, typename Transform>
		requires JNITransform<Transform, std::ranges::range_value_t<Range>, jobject>
	jobjectArray CreateObjectArray(JNIEnv* env, jclass elementClass, Range&& range, Transform&& transform)
	{
		auto size = std::ranges::size(range);

		jobjectArray array = env->NewObjectArray(
			static_cast<jsize>(size),
			elementClass,
			nullptr);

		jsize index = 0;
		for (auto&& item : range)
		{
			jobject obj = transform(item);
			env->SetObjectArrayElement(array, index++, obj);
			env->DeleteLocalRef(obj);
		}

		return array;
	}

	template<std::ranges::input_range Range, typename Transform>
		requires(!std::ranges::sized_range<Range> && JNITransform<Transform, std::ranges::range_value_t<Range>, jobject>)
	jobjectArray CreateObjectArray(JNIEnv* env, jclass elementClass, Range&& range, Transform&& transform)
	{
		std::vector<std::ranges::range_value_t<Range>> vector;

		for (auto&& e : range)
		{
			vector.push_back(static_cast<decltype(e)&&>(e));
		}

		return CreateObjectArray(env, elementClass, vector, std::forward<Transform>(transform));
	}

	template<std::ranges::sized_range Range>
	jobjectArray CreateStringObjectArray(JNIEnv* env, Range&& range)
		requires std::same_as<std::ranges::range_value_t<Range>, std::string>
	{
		jclass elementClass = env->FindClass("java/lang/String");

		jobjectArray array = CreateObjectArray(
			env,
			elementClass,
			range,
			[env](const std::string& str) -> jstring { return env->NewStringUTF(str.c_str()); });

		env->DeleteLocalRef(elementClass);

		return array;
	}

	template<std::ranges::input_range Range>
		requires(!std::ranges::sized_range<Range> && std::same_as<std::ranges::range_value_t<Range>, std::string>)
	jobjectArray CreateStringObjectArray(JNIEnv* env, Range&& range)
	{
		std::vector<std::ranges::range_value_t<Range>> vector;

		for (auto&& e : range)
		{
			vector.push_back(static_cast<decltype(e)&&>(e));
		}

		return CreateStringObjectArray(env, vector);
	}

	template<typename... TArgs>
	jobject NewObject(JNIEnv* env, const char* className, const std::string& ctrSig = "()V", TArgs&&... args)
	{
		jclass javaClass = env->FindClass(className);
		jmethodID ctrId = env->GetMethodID(javaClass, "<init>", ctrSig.c_str());
		jobject obj = env->NewObject(javaClass, ctrId, std::forward<TArgs>(args)...);
		env->DeleteLocalRef(javaClass);
		return obj;
	}

	// Runs `func` on a long-lived worker thread that stays attached to the JVM, and blocks the calling
	// thread until `func` has finished executing (same synchronous semantics callers already rely on).
	//
	// This used to spawn a brand new std::jthread for every single call (and immediately join it, since the
	// jthread was an unnamed temporary destroyed at the end of the statement). That is not just wasteful -
	// input/rumble callbacks can fire many times per second, and creating+joining a native OS thread for each
	// one risks exhausting the process' thread budget on a phone under memory/CPU pressure. When
	// pthread_create() fails, std::jthread's constructor throws std::system_error, and since nothing here
	// caught it, that exception would propagate out uncaught and call std::terminate() - crashing the app.
	// A single persistent worker thread removes both the overhead and that crash path entirely.
	class JNIWorkerThread
	{
	  public:
		static JNIWorkerThread& GetInstance()
		{
			static JNIWorkerThread instance;
			return instance;
		}

		void RunAndWait(std::invocable<JNIEnv*> auto func)
		{
			if (t_isWorkerThread)
			{
				// called recursively from within the worker thread itself - running it directly avoids a
				// deadlock (the worker can't service a queued task while it's blocked waiting on itself)
				func(GetEnv());
				return;
			}

			std::exception_ptr capturedException;
			bool done = false;
			{
				std::unique_lock lock(m_mutex);
				m_queue.emplace_back([&]() {
					try
					{
						func(GetEnv());
					}
					catch (...)
					{
						capturedException = std::current_exception();
					}
					{
						std::lock_guard doneLock(m_mutex);
						done = true;
					}
					m_condVar.notify_all();
				});
			}
			m_condVar.notify_all();

			std::unique_lock lock(m_mutex);
			m_condVar.wait(lock, [&]() { return done; });
			lock.unlock();

			if (capturedException)
				std::rethrow_exception(capturedException);
		}

	  private:
		JNIWorkerThread()
		{
			m_thread = std::thread([this]() { WorkerLoop(); });
		}

		// intentionally never joined/stopped: this is a process-lifetime singleton worker

		void WorkerLoop()
		{
			t_isWorkerThread = true;
			while (true)
			{
				std::function<void()> task;
				{
					std::unique_lock lock(m_mutex);
					m_condVar.wait(lock, [&]() { return !m_queue.empty(); });
					task = std::move(m_queue.front());
					m_queue.pop_front();
				}
				task();
			}
		}

		static inline thread_local bool t_isWorkerThread = false;
		std::thread m_thread;
		std::mutex m_mutex;
		std::condition_variable m_condVar;
		std::deque<std::function<void()>> m_queue;
	};

	inline void FiberSafeJNICall(std::invocable<JNIEnv*> auto func)
	{
		JNIWorkerThread::GetInstance().RunAndWait(func);
	}
} // namespace JNIUtils
