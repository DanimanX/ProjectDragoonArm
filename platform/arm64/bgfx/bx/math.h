#ifndef PROJECTDRAGOON_BX_MATH_H
#define PROJECTDRAGOON_BX_MATH_H
#include <math.h>
namespace bx
{
	inline void mtxOrtho(float* _result, float _left, float _right, float _bottom, float _top, float _near, float _far, float _offset = 0.0f)
	{
		const float width  = (_right - _left);
		const float height = (_top - _bottom);
		const float invZ   = 1.0f / (_far - _near);
		_result[0]  = 2.0f / width;
		_result[1]  = 0.0f;
		_result[2]  = 0.0f;
		_result[3]  = 0.0f;
		_result[4]  = 0.0f;
		_result[5]  = 2.0f / height;
		_result[6]  = 0.0f;
		_result[7]  = 0.0f;
		_result[8]  = 0.0f;
		_result[9]  = 0.0f;
		_result[10] = -2.0f * invZ;
		_result[11] = 0.0f;
		_result[12] = -(_right + _left) / width;
		_result[13] = -(_top + _bottom) / height;
		_result[14] = -(_far + _near) * invZ + _offset;
		_result[15] = 1.0f;
	}
	inline void mtxIdentity(float* _result) { for (int i = 0; i < 16; i++) _result[i] = (i % 5) == 0 ? 1.0f : 0.0f; }
}
#endif
