// bx/bx.h — minimal bx subset used by the Azel engine (ProjectDragoonArm).
#ifndef PROJECTDRAGOON_BX_H
#define PROJECTDRAGOON_BX_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Platform macros must be visible everywhere bx is: the engine's embedded
// shader table gates its ESSL/GLSL entries on BX_PLATFORM_*.
#include <bx/platform.h>

#define BX_UNUSED(x) ((void)(x))
#define BX_COUNTOF(_arr) (sizeof(_arr)/sizeof(0[_arr]))
#define BX_CONCATENATE(_x, _y) BX_CONCATENATE_(_x, _y)
#define BX_CONCATENATE_(_x, _y) _x##_y

namespace bx
{
	template <typename To, typename From>
	inline To bitCast(const From& _from) { To to; memcpy(&to, &_from, sizeof(To)); return to; }

	template <typename To, typename From>
	inline To narrowCast(From _from) { return (To)_from; }

	template <typename Ty>
	inline Ty min(Ty _a, Ty _b) { return _a < _b ? _a : _b; }

	template <typename Ty>
	inline Ty max(Ty _a, Ty _b) { return _a > _b ? _a : _b; }

	inline void memCopy(void* _dst, const void* _src, size_t _numBytes) { memcpy(_dst, _src, _numBytes); }
	inline void memSet(void* _dst, uint8_t _ch, size_t _numBytes) { memset(_dst, _ch, _numBytes); }
	inline int32_t strCmp(const char* _a, const char* _b, int32_t _max = INT32_MAX) { return strncmp(_a, _b, (size_t)_max); }
	inline size_t strLen(const char* _a) { return strlen(_a); }

	struct AllocatorI
	{
		virtual ~AllocatorI() {}
		virtual void* alloc(size_t _size, size_t _align = 16, const char* _file = NULL, uint32_t _line = 0) = 0;
		virtual void free(void* _ptr, size_t _align = 16, const char* _file = NULL, uint32_t _line = 0) = 0;
		virtual size_t realloc(void* _ptr, size_t _size, size_t _align, const char* _file, uint32_t _line) = 0;
	};

	struct DefaultAllocator : public AllocatorI
	{
		void* alloc(size_t _size, size_t _align, const char*, uint32_t) override;
		void free(void* _ptr, size_t _align, const char*, uint32_t) override;
		size_t realloc(void* _ptr, size_t _size, size_t _align, const char*, uint32_t) override;
	};

	inline void* alloc(AllocatorI* _allocator, size_t _size) { return _allocator->alloc(_size, 16, NULL, 0); }
	inline void free(AllocatorI* _allocator, void* _ptr) { _allocator->free(_ptr, 16, NULL, 0); }

	template <typename Ty>
	inline Ty gather(const Ty& _default, bool _condition, const Ty& _value) { return _condition ? _value : _default; }
}

#endif
