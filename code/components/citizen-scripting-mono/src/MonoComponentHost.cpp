/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include <om/OMComponent.h>
#include <ResourceManager.h>
#include <fxScripting.h>
#include <chrono>

#include <Error.h>

#include <mono/jit/jit.h>
#include <mono/utils/mono-logger.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/profiler.h>
#include <mono/metadata/mono-gc.h>
#include <glib.h>
#include <sstream>

#ifndef IS_FXSERVER
extern "C"
{
#include <mono/metadata/security-core-clr.h>
}
#endif

static MonoDomain* g_rootDomain;

#ifndef IS_FXSERVER
const static wchar_t* const g_platformAssemblies[] =
{
	L"mscorlib.dll",
	L"System.dll",
	L"System.Core.dll",
	L"CitizenFX.Core.dll",
	L"Mono.CSharp.dll"
};

enum {
	TYPE_ALLOC,
	TYPE_GC,
	TYPE_METADATA,
	TYPE_METHOD,
	TYPE_EXCEPTION,
	TYPE_MONITOR,
	TYPE_HEAP,
	TYPE_SAMPLE,
	TYPE_RUNTIME,
	TYPE_COVERAGE,
	TYPE_META,
	/* extended type for TYPE_HEAP */
	TYPE_HEAP_START = 0 << 4,
	TYPE_HEAP_END = 1 << 4,
	TYPE_HEAP_OBJECT = 2 << 4,
	TYPE_HEAP_ROOT = 3 << 4,
	/* extended type for TYPE_METADATA */
	TYPE_END_LOAD = 2 << 4,
	TYPE_END_UNLOAD = 4 << 4,
	/* extended type for TYPE_GC */
	TYPE_GC_EVENT = 1 << 4,
	TYPE_GC_RESIZE = 2 << 4,
	TYPE_GC_MOVE = 3 << 4,
	TYPE_GC_HANDLE_CREATED = 4 << 4,
	TYPE_GC_HANDLE_DESTROYED = 5 << 4,
	TYPE_GC_HANDLE_CREATED_BT = 6 << 4,
	TYPE_GC_HANDLE_DESTROYED_BT = 7 << 4,
	TYPE_GC_FINALIZE_START = 8 << 4,
	TYPE_GC_FINALIZE_END = 9 << 4,
	TYPE_GC_FINALIZE_OBJECT_START = 10 << 4,
	TYPE_GC_FINALIZE_OBJECT_END = 11 << 4,
	/* extended type for TYPE_METHOD */
	TYPE_LEAVE = 1 << 4,
	TYPE_ENTER = 2 << 4,
	TYPE_EXC_LEAVE = 3 << 4,
	TYPE_JIT = 4 << 4,
	/* extended type for TYPE_EXCEPTION */
	TYPE_THROW_NO_BT = 0 << 7,
	TYPE_THROW_BT = 1 << 7,
	TYPE_CLAUSE = 1 << 4,
	/* extended type for TYPE_ALLOC */
	TYPE_ALLOC_NO_BT = 0 << 4,
	TYPE_ALLOC_BT = 1 << 4,
	/* extended type for TYPE_MONITOR */
	TYPE_MONITOR_NO_BT = 0 << 7,
	TYPE_MONITOR_BT = 1 << 7,
	/* extended type for TYPE_SAMPLE */
	TYPE_SAMPLE_HIT = 0 << 4,
	TYPE_SAMPLE_USYM = 1 << 4,
	TYPE_SAMPLE_UBIN = 2 << 4,
	TYPE_SAMPLE_COUNTERS_DESC = 3 << 4,
	TYPE_SAMPLE_COUNTERS = 4 << 4,
	/* extended type for TYPE_RUNTIME */
	TYPE_JITHELPER = 1 << 4,
	/* extended type for TYPE_COVERAGE */
	TYPE_COVERAGE_ASSEMBLY = 0 << 4,
	TYPE_COVERAGE_METHOD = 1 << 4,
	TYPE_COVERAGE_STATEMENT = 2 << 4,
	TYPE_COVERAGE_CLASS = 3 << 4,
	/* extended type for TYPE_META */
	TYPE_SYNC_POINT = 0 << 4,
	TYPE_END
};
enum {
	/* metadata type byte for TYPE_METADATA */
	TYPE_CLASS = 1,
	TYPE_IMAGE = 2,
	TYPE_ASSEMBLY = 3,
	TYPE_DOMAIN = 4,
	TYPE_THREAD = 5,
	TYPE_CONTEXT = 6,
};

template<typename InputIt>
std::string join(InputIt begin,
	InputIt end,
	const std::string & separator = ", ",  // see 1.
	const std::string & concluder = "")    // see 1.
{
	std::ostringstream ss;

	if (begin != end)
	{
		ss << *begin++; // see 3.
	}

	while (begin != end) // see 3.
	{
		ss << separator;
		ss << *begin++;
	}

	ss << concluder;
	return ss.str();
}

static int CoreClrCallback(const char* imageName)
{
	if (!imageName)
	{
		return FALSE;
	}

	wchar_t* filePart = nullptr;
	wchar_t fullPath[512];

	if (GetFullPathNameW(ToWide(imageName).c_str(), _countof(fullPath), fullPath, &filePart) == 0)
	{
		return FALSE;
	}

	if (!filePart)
	{
		return FALSE;
	}

	*(filePart - 1) = '\0';

	std::wstring platformPath = MakeRelativeCitPath(L"citizen\\clr2\\lib");

	if (_wcsicmp(platformPath.c_str(), fullPath) != 0)
	{
		platformPath = MakeRelativeCitPath(L"citizen\\clr2\\lib\\mono\\4.5");

		if (_wcsicmp(platformPath.c_str(), fullPath) != 0)
		{
			trace("%s %s is not a platform image.\n", ToNarrow(fullPath), ToNarrow(filePart));
			return FALSE;
		}
	}

	for (int i = 0; i < _countof(g_platformAssemblies); i++)
	{
		if (!_wcsicmp(filePart, g_platformAssemblies[i]))
		{
			return TRUE;
		}
	}

	trace("%s %s is not a platform image (even though the dir matches).\n", ToNarrow(fullPath), ToNarrow(filePart));

	return FALSE;
}
#endif

static void OutputExceptionDetails(MonoObject* exc)
{
	MonoClass* eclass = mono_object_get_class(exc);

	if (eclass)
	{
		MonoObject* toStringExc = nullptr;
		MonoString* msg = mono_object_to_string(exc, &toStringExc);

		MonoProperty* prop = mono_class_get_property_from_name(eclass, "StackTrace");
		MonoMethod* getter = mono_property_get_get_method(prop);
		MonoString* msg2 = (MonoString*)mono_runtime_invoke(getter, exc, NULL, NULL);

		if (toStringExc)
		{
			MonoProperty* prop = mono_class_get_property_from_name(eclass, "Message");
			MonoMethod* getter = mono_property_get_get_method(prop);
			msg = (MonoString*)mono_runtime_invoke(getter, exc, NULL, NULL);
		}

		GlobalError("Unhandled exception in Mono script environment: %s %s", mono_string_to_utf8(msg), mono_string_to_utf8(msg2));
	}
}

static void GI_PrintLogCall(MonoString* str)
{
	trace("%s", mono_string_to_utf8(str));
}

MonoMethod* g_getImplementsMethod;
MonoMethod* g_createObjectMethod;

static inline std::string MakeRelativeNarrowPath(const std::string& path)
{
#ifdef _WIN32
	return ToNarrow(MakeRelativeCitPath(ToWide(path)));
#else
	return MakeRelativeCitPath(path);
#endif
}

static uint64_t current_time(void)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct _MonoProfiler {
	std::map<std::pair<std::string, std::string>, uint64_t> gchandles;
	HANDLE mutex; /* used to ensure only one thread accesses the arrays */
	BOOLEAN do_heap_walk;
	uint64_t last_hs_time;

	std::set<MonoClass*> logClasses;
	std::stringbuf* fileBuffer;
	std::stringbuf* logBuffer; //current log buffer
	uint64_t obj_base;
	uint64_t ptr_base;

	std::map<std::pair<std::string, std::string>, uint64_t> CopyTraces();
	void Clear();
	void init();
	void gc_event(MonoGCEvent ev, int generation);
	void heap_walk();
	static void gc_event_static(MonoProfiler *profiler, MonoGCEvent ev, int generation);
	int gc_reference(MonoObject *obj, MonoClass *klass, uintptr_t size, uintptr_t num, MonoObject **refs, uintptr_t *offsets);
	static int gc_reference_static(MonoObject *obj, MonoClass *klass, uintptr_t size, uintptr_t num, MonoObject **refs, uintptr_t *offsets, void *data);
};
typedef _MonoProfiler MonoProfiler;

mono_bool walk_stack(MonoMethod *method, int32_t native_offset, int32_t il_offset, mono_bool managed, void* data) {
	auto list = (std::vector<std::string>*)data;
	if (managed) {
		auto methodName = std::string(mono_method_full_name(method, true));
		list->push_back(methodName);
	}
	else {
		list->push_back("<native code>");
	}
	return false;
}
void gc_allocation(MonoProfiler *prof, MonoObject *obj, MonoClass *klass) {
	auto name = std::string(mono_type_get_name(mono_class_get_type(klass)));
	if (name == "System.Threading.Tasks.Task<System.Threading.Tasks.Task>") {
		WaitForSingleObject(prof->mutex, -1);

		std::vector<std::string> list;
		mono_stack_walk_no_il(walk_stack, &list);
		auto traceStr = join(list.begin(), list.end(), "\n");
		auto key = std::make_pair(name, traceStr);
		auto ele = prof->gchandles.find(key);
		prof->gchandles.insert_or_assign(key, ele != prof->gchandles.end() ? ele->second + 1 : 1);

		ReleaseMutex(prof->mutex);
	}

}
void gc_roots(MonoProfiler *prof, int num, void **objects, int *root_types, uintptr_t *extra_info);

static MonoProfiler g_profiler;
void _MonoProfiler::init()
{
	//type_name = g_getenv("GCHANDLES_FOR_TYPE");
	mutex = CreateMutex(NULL, false, NULL);
	/* Register the profiler with mono */
	mono_profiler_install(this, NULL);
	/* Supply a function for the gc_roots hook */
	mono_profiler_install_gc(gc_event_static, NULL);
	//mono_profiler_install_allocation(gc_allocation);
	mono_profiler_install_gc_roots(nullptr, gc_roots);
	/* Enable gc_roots tracking in the profiler so our hook is invoked */
	mono_profiler_set_events((MonoProfileFlags)(MONO_PROFILE_GC | MONO_PROFILE_ALLOCATIONS | MONO_PROFILE_GC_ROOTS));
	obj_base = 0;
	ptr_base = 0;
	logBuffer = nullptr;
	fileBuffer = nullptr;
	last_hs_time = current_time();
}
template <class Type>
void put(std::stringbuf* buf, const Type& var)
{
	buf->sputn(reinterpret_cast<const char*>(&var), sizeof var);
}
static void encode_uleb128(uint64_t value, std::stringbuf* buffer)
{
	do {
		uint8_t b = value & 0x7f;
		value >>= 7;

		if (value != 0) /* more bytes to come */
			b |= 0x80;

		put(buffer, b);
	} while (value);
}
static void encode_sleb128(intptr_t value, std::stringbuf* buffer)
{
	int more = 1;
	int negative = (value < 0);
	unsigned int size = sizeof(intptr_t) * 8;
	uint8_t byte;

	while (more) {
		byte = value & 0x7f;
		value >>= 7;

		/* the following is unnecessary if the
		* implementation of >>= uses an arithmetic rather
		* than logical shift for a signed left operand
		*/
		if (negative)
			/* sign extend */
			value |= -((intptr_t)1 << (size - 7));

		/* sign bit of byte is second high order bit (0x40) */
		if ((value == 0 && !(byte & 0x40)) ||
			(value == -1 && (byte & 0x40)))
			more = 0;
		else
			byte |= 0x80;

		put(buffer, byte);
	}
}
static void emit_byte(std::stringbuf* buffer, int value)
{
	put(buffer, (char)value);
}

static void emit_value(std::stringbuf* logbuffer, int value)
{
	encode_uleb128(value, logbuffer);
}

static void
emit_time(std::stringbuf* logbuffer, uint64_t value)
{
	//uint64_t tdiff = value - logbuffer->last_time;
	encode_uleb128(value, logbuffer);
}
static void
emit_event_time(std::stringbuf *logbuffer, int event, uint64_t time)
{
	emit_byte(logbuffer, event);
	emit_time(logbuffer, time);
}

static void emit_event(std::stringbuf* logbuffer, int event)
{
	emit_event_time(logbuffer, event, current_time());
}
static void emit_svalue(std::stringbuf *logbuffer, int64_t value)
{
	encode_sleb128(value, logbuffer);
}

static void emit_obj(std::stringbuf* logbuffer, void *ptr)
{
	if (!g_profiler.obj_base)
		g_profiler.obj_base = (uintptr_t)ptr >> 3;

	emit_svalue(logbuffer, ((uintptr_t)ptr >> 3) - g_profiler.obj_base);
}

static void emit_ptr(std::stringbuf* logbuffer, void *ptr)
{
	if (!g_profiler.ptr_base)
		g_profiler.ptr_base = (uintptr_t)ptr;;

	emit_svalue(logbuffer, (intptr_t)ptr - g_profiler.ptr_base);
}

std::map<std::pair<std::string, std::string>, uint64_t> _MonoProfiler::CopyTraces() {


	WaitForSingleObject(this->mutex, -1);

	std::map<std::pair<std::string, std::string>, uint64_t> ret(this->gchandles);

	ReleaseMutex(this->mutex);

	return ret;
}
void _MonoProfiler::Clear() {
	WaitForSingleObject(this->mutex, -1);

	this->gchandles.clear();

	ReleaseMutex(this->mutex);
}
void _MonoProfiler::gc_event_static(MonoProfiler *profiler, MonoGCEvent ev, int generation) {
	profiler->gc_event(ev, generation);
}
int _MonoProfiler::gc_reference_static(MonoObject *obj, MonoClass *klass, uintptr_t size, uintptr_t num, MonoObject **refs, uintptr_t *offsets, void *data) {
	MonoProfiler* that = (MonoProfiler*)data;
	return that->gc_reference(obj, klass, size, num, refs, offsets);

}
int _MonoProfiler::gc_reference(MonoObject *obj, MonoClass *klass, uintptr_t size, uintptr_t num, MonoObject **refs, uintptr_t *offsets) {
	auto logbuffer = this->logBuffer;
	size += 7;
	size &= ~7;

	this->logClasses.insert(klass);

	emit_event(logbuffer, TYPE_HEAP_OBJECT | TYPE_HEAP);
	emit_obj(logbuffer, obj);
	emit_ptr(logbuffer, klass);
	emit_value(logbuffer, size);
	emit_value(logbuffer, num);

	uintptr_t last_offset = 0;

	for (int i = 0; i < num; ++i) {
		emit_value(logbuffer, offsets[i] - last_offset);
		last_offset = offsets[i];
		emit_obj(logbuffer, refs[i]);
	}

	return 0;
}

static int Id = 0;
static void dump_header(std::stringbuf* buf)
{
	put<int32_t>(buf, 0x4D505A01);
	put<char>(buf, 1);
	put<char>(buf, 1);
	put<char>(buf, 0);
	put<char>(buf, 8);
	put<int64_t>(buf, 0);
	put<int32_t>(buf, 0);
	put<int32_t>(buf, 0);
	put<int32_t>(buf, 0);
	put<int32_t>(buf, 0);
}
static void dump_buffer(std::stringbuf* buf, std::stringbuf* logbuffer, MonoProfiler *profiler)
{
	auto logBuf = logbuffer->str();

	put<int32_t>(buf, 0x4D504C01);
	put<int32_t>(buf, logBuf.size());
	put<int64_t>(buf, current_time());

	put<int64_t>(buf, profiler->ptr_base);
	put<int64_t>(buf, profiler->obj_base);
	put<int64_t>(buf, 1);
	put<int64_t>(buf, 0);

	buf->sputn(logBuf.c_str(), logBuf.size());
}
static void dump_classes(std::stringbuf* buf, std::set<MonoClass*>& classes)
{
	for (auto it = classes.begin(); it != classes.end(); ++it) {
		auto klass = *it;
		char *name = mono_type_get_name(mono_class_get_type(klass));
		int nlen = strlen(name);
		MonoImage *image = mono_class_get_image(klass);

		emit_event(buf, TYPE_END_LOAD | TYPE_METADATA);
		emit_byte(buf, TYPE_CLASS);
		emit_ptr(buf, klass);
		emit_ptr(buf, image);
		emit_value(buf, 0);

		buf->sputn(name, nlen);
		buf->sputc(0);
	}

}
static void gc_roots(MonoProfiler *prof, int num, void **objects, int *root_types, uintptr_t *extra_info)
{
	if (prof->logBuffer == nullptr)
		return;

	auto buf = prof->logBuffer;

	emit_event(buf, TYPE_HEAP_ROOT | TYPE_HEAP);
	emit_value(buf, num);
	emit_value(buf, mono_gc_collection_count(mono_gc_max_generation()));

	for (int i = 0; i < num; ++i) {
		emit_obj(buf, objects[i]);
		emit_byte(buf, root_types[i]);
		emit_value(buf, extra_info[i]);
	}
}
void _MonoProfiler::heap_walk() {
	auto logbuffer = this->logBuffer;
	emit_event(logbuffer, TYPE_HEAP_START | TYPE_HEAP);
	mono_gc_walk_heap(0, gc_reference_static, this);
	dump_classes(logbuffer, this->logClasses);
	emit_event(logbuffer, TYPE_HEAP_END | TYPE_HEAP);

}
void _MonoProfiler::gc_event(MonoGCEvent ev, int generation)
{
	std::string fileBuf; FILE* fptr;
	if (generation == mono_gc_max_generation())
		trace("GC EVENT[%d]: %d\n", generation, (int)ev);
	switch (ev) {
	case MONO_GC_EVENT_PRE_STOP_WORLD:
		if (this->do_heap_walk && generation == mono_gc_max_generation()) {
			this->logBuffer = new std::stringbuf();
			this->fileBuffer = new std::stringbuf();
			this->obj_base = 0;
			this->ptr_base = 0;
			this->logClasses.clear();

		}
		break;
	case MONO_GC_EVENT_PRE_START_WORLD:
		if (generation == mono_gc_max_generation())
			trace("[GC] A collection(%d)", mono_gc_max_generation());
		if (this->fileBuffer != nullptr && generation == mono_gc_max_generation()) {
			trace("Walking heap");

			this->heap_walk();

			do_heap_walk = FALSE;
		}
		break;
	case MONO_GC_EVENT_POST_START_WORLD_UNLOCKED:
		if (this->fileBuffer != nullptr && generation == mono_gc_max_generation()) {
			trace("dumping buffer");
			dump_buffer(this->fileBuffer, this->logBuffer, this);

			trace("Writing heap to event file");
			fptr = fopen("test.mlpd", "ab+");
			fileBuf = this->fileBuffer->str();
			fwrite(fileBuf.c_str(), 1, fileBuf.length(), fptr);
			fflush(fptr);
			fclose(fptr);

			delete this->fileBuffer;
			delete this->logBuffer;
			this->fileBuffer = nullptr;
			this->logBuffer = nullptr;

			trace("finished heap dump");
		}

		break;
	default:
		break;
	}
}

static void InitMono()
{
	trace("Mono Ver: %s", mono_get_runtime_build_info());

	std::string citizenClrPath = MakeRelativeNarrowPath("citizen/clr2/lib/");
	std::string citizenCfgPath = MakeRelativeNarrowPath("citizen/clr2/cfg/");

	mono_set_dirs(citizenClrPath.c_str(), citizenCfgPath.c_str());

#ifdef _WIN32
	std::wstring citizenClrLibPath = MakeRelativeCitPath(L"citizen/clr2/lib/mono/4.5/");

	SetEnvironmentVariable(L"MONO_PATH", citizenClrLibPath.c_str());

	mono_set_crash_chaining(true);
#else
	std::string citizenClrLibPath = MakeRelativeNarrowPath("citizen/clr2/lib/mono/4.5/");

	putenv(const_cast<char*>(va("MONO_PATH=%s", citizenClrLibPath)));
#endif

	mono_assembly_setrootdir(citizenClrPath.c_str());

	putenv("MONO_DEBUG=casts");

	char* args[1];

#ifdef _WIN32
	args[0] = "--soft-breakpoints";
#else
	args[0] = "--use-fallback-tls";
#endif

	mono_jit_parse_options(1, args);

	g_profiler.init();

	mono_debug_init(MONO_DEBUG_FORMAT_MONO);


	trace("Initializing Mono\n");

	g_rootDomain = mono_jit_init_version("Citizen", "v4.0.30319");

	mono_domain_set_config(g_rootDomain, ".", "cfx.config");

	trace("Initializing Mono completed\n");

	mono_install_unhandled_exception_hook([](MonoObject* exc, void*)
	{
		OutputExceptionDetails(exc);
	}, nullptr);

	mono_set_crash_chaining(true);

	mono_add_internal_call("CitizenFX.Core.GameInterface::PrintLog", reinterpret_cast<void*>(GI_PrintLogCall));
	mono_add_internal_call("CitizenFX.Core.GameInterface::fwFree", reinterpret_cast<void*>(fwFree));

	std::string platformPath = MakeRelativeNarrowPath("citizen/clr2/lib/mono/4.5/CitizenFX.Core.dll");

	auto scriptManagerAssembly = mono_domain_assembly_open(g_rootDomain, platformPath.c_str());

	if (!scriptManagerAssembly)
	{
		FatalError("Could not load CitizenFX.Core.dll.\n");
	}

	auto scriptManagerImage = mono_assembly_get_image(scriptManagerAssembly);

	bool methodSearchSuccess = true;
	MonoMethodDesc* description;

#define method_search(name, method) description = mono_method_desc_new(name, 1); \
			method = mono_method_desc_search_in_image(description, scriptManagerImage); \
			mono_method_desc_free(description); \
			methodSearchSuccess = methodSearchSuccess && method != NULL

	MonoMethod* rtInitMethod;
	method_search("CitizenFX.Core.RuntimeManager:Initialize", rtInitMethod);
	method_search("CitizenFX.Core.RuntimeManager:GetImplementedClasses", g_getImplementsMethod);
	method_search("CitizenFX.Core.RuntimeManager:CreateObjectInstance", g_createObjectMethod);

	if (!methodSearchSuccess)
	{
		FatalError("Couldn't find one or more CitizenFX.Core methods.\n");
	}

	MonoObject* exc = nullptr;
	mono_runtime_invoke(rtInitMethod, nullptr, nullptr, &exc);

	if (exc)
	{
		OutputExceptionDetails(exc);
		return;
	}
}

struct MonoAttachment
{
	MonoThread* thread;

	MonoAttachment()
		: thread(nullptr)
	{
		if (!mono_domain_get())
		{
			thread = mono_thread_attach(g_rootDomain);
		}
	}

	~MonoAttachment()
	{
		if (thread)
		{
			mono_thread_detach(thread);
			thread = nullptr;
		}
	}
};

static void MonoEnsureThreadAttached()
{
	static thread_local MonoAttachment attachment;
}

result_t MonoCreateObjectInstance(const guid_t& guid, const guid_t& iid, void** objectRef)
{
	MonoEnsureThreadAttached();

	MonoObject* exc = nullptr;

	guid_t lguid = guid;
	guid_t liid = iid;

	void* args[2];
	args[0] = &lguid;
	args[1] = &liid;

	auto retval = mono_runtime_invoke(g_createObjectMethod, nullptr, args, &exc);

	if (exc)
	{
		return FX_E_NOINTERFACE;
	}

	*objectRef = *(void**)(mono_object_unbox(retval));

	if (!*objectRef)
	{
		return FX_E_NOINTERFACE;
	}

	return FX_S_OK;
}

std::vector<guid_t> MonoGetImplementedClasses(const guid_t& iid)
{
	MonoEnsureThreadAttached();

	void* args[1];
	args[0] = (char*)&iid;

	MonoObject* exc = nullptr;
	MonoArray* retval = (MonoArray*)mono_runtime_invoke(g_getImplementsMethod, nullptr, args, &exc);

	if (exc)
	{
		return std::vector<guid_t>();
	}

	guid_t* retvalStart = mono_array_addr(retval, guid_t, 0);
	uintptr_t retvalLength = mono_array_length(retval);

	return std::vector<guid_t>(retvalStart, retvalStart + retvalLength);
}
using namespace std::chrono;
class GcDumpResource : public fwRefCountable
{
public:
	int64_t lastLog;
	int64_t lastStackLog;
	GcDumpResource(fx::Resource* resource)
	{
		lastLog = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
		resource->OnTick.Connect([=]() { StartTick(resource); }, -10000000);
		resource->OnTick.Connect([=]() { EndTick(); }, 10000000);
	}

private:
	void StartTick(fx::Resource* resource)
	{
		auto msNow = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
		if (msNow - lastLog > 2 * 60 * 1000) {
			trace("[GC] Collecting %d", mono_gc_max_generation());
			g_profiler.do_heap_walk = true;
			mono_gc_collect(mono_gc_max_generation());
			trace("[GC] Collected %d", mono_gc_max_generation());
			lastLog = msNow;
		}

		if (msNow - lastStackLog > 1 * 60 * 1000) {
			auto traces = g_profiler.CopyTraces();
			g_profiler.Clear();

			std::vector<std::tuple<std::string, std::string, uint64_t>> list;
			for (auto it = traces.begin(); it != traces.end(); ++it) {
				list.push_back(std::make_tuple(it->first.first, it->first.second, it->second));
			}

			std::sort(list.begin(), list.end(), [](std::tuple<std::string, std::string, uint64_t> left, std::tuple<std::string, std::string, uint64_t> right) { return std::get<2>(left) > std::get<2>(right);  });

			std::string output;
			char buffer[50000];
			for (auto i = 0; i < list.size(); i++) {
				auto len = std::snprintf(buffer, 50000, "Called: %d, Caller: %s, Trace: %s", std::get<2>(list[i]), std::get<0>(list[i]).c_str(), std::get<1>(list[i]).c_str());
				output += std::string(buffer);
				output += "\n\n\n\n";
				trace("aaa: %d", len);
			}

			auto file = fopen("stack.txt", "wb");
			fwrite(output.c_str(), 1, output.size(), file);
			fclose(file);

			lastStackLog = msNow;
		}
	}
	void EndTick()
	{
	}
};

DECLARE_INSTANCE_TYPE(GcDumpResource);

static InitFunction initFunction([]()
{
	//create a new dump file with header
	std::stringbuf filebuffer;
	dump_header(&filebuffer);
	auto fptr = fopen("test.mlpd", "wb+");
	auto fileBuf = filebuffer.str();
	fwrite(fileBuf.c_str(), 1, fileBuf.length(), fptr);
	fflush(fptr);
	fclose(fptr);

	InitMono();

	fx::Resource::OnInitializeInstance.Connect([](fx::Resource* resource)
	{
		if (resource->GetName() == "_cfx_internal") {
			resource->SetComponent(new GcDumpResource(resource));
		}
	});
});
