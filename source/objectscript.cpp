#include "objectscript.h"
#include "os-binder.h"
#include <time.h>

using namespace ObjectScript;

#define HASH_GROW_SHIFT 0

#define MAX_GENERIC_CONST_INDEX 143

#define Instruction OS_U32

/*
** size and position of opcode arguments.
*/
#define SIZE_C		9
#define SIZE_B		9
#define SIZE_Bx		(SIZE_C + SIZE_B)
#define SIZE_A		8
#define SIZE_Ax		(SIZE_C + SIZE_B + SIZE_A)

#define SIZE_OP		6

#define POS_OP		0
#define POS_A		(POS_OP + SIZE_OP)
#define POS_C		(POS_A + SIZE_A)
#define POS_B		(POS_C + SIZE_C)
#define POS_Bx		POS_C
#define POS_Ax		POS_A

#define MAXARG_Bx        ((1<<SIZE_Bx)-1)
#define MAXARG_sBx        (MAXARG_Bx>>1)         /* `sBx' is signed */
#define MAXARG_Ax	((1<<SIZE_Ax)-1)

#define MAXARG_A        ((1<<SIZE_A)-1)
#define MAXARG_B        ((1<<SIZE_B)-1)
#define MAXARG_C        ((1<<SIZE_C)-1)

/* creates a mask with `n' 1 bits at position `p' */
#define MASK1(n,p)	((~((~(Instruction)0)<<(n)))<<(p))

/* creates a mask with `n' 0 bits at position `p' */
#define MASK0(n,p)	(~MASK1(n,p))

/*
** the following macros help to manipulate instructions
*/

#define GET_OPCODE(i)	(((i)>>POS_OP) & MASK1(SIZE_OP, 0))
#define SET_OPCODE(i,o)	((i) = (((i) & MASK0(SIZE_OP, POS_OP)) | (((Instruction)(o))<<POS_OP) & MASK1(SIZE_OP, POS_OP)))

#define getarg(i,pos,size)		(((i)>>pos) & MASK1(size, 0))
#define setarg(i,v,pos,size)	((i) = (((i)&MASK0(size,pos)) | (((Instruction)(v))<<pos) & MASK1(size, pos)))

#define GETARG_A(i)		getarg(i, POS_A, SIZE_A)
#define SETARG_A(i,v)	setarg(i, v, POS_A, SIZE_A)

#define GETARG_B(i)		getarg(i, POS_B, SIZE_B)
#define SETARG_B(i,v)	setarg(i, v, POS_B, SIZE_B)

#define GETARG_C(i)		getarg(i, POS_C, SIZE_C)
#define SETARG_C(i,v)	setarg(i, v, POS_C, SIZE_C)

#define GETARG_Bx(i)	getarg(i, POS_Bx, SIZE_Bx)
#define SETARG_Bx(i,v)	setarg(i, v, POS_Bx, SIZE_Bx)

#define GETARG_Ax(i)	getarg(i, POS_Ax, SIZE_Ax)
#define SETARG_Ax(i,v)	setarg(i, v, POS_Ax, SIZE_Ax)

#define GETARG_sBx(i)	(GETARG_Bx(i)-MAXARG_sBx)
#define SETARG_sBx(i,b)	SETARG_Bx((i),((unsigned int)((b)+MAXARG_sBx)))


#define OS_OPCODE_ABC(o,a,b,c)	((((Instruction)(o)) << POS_OP) \
			| (((Instruction)(a)) << POS_A) \
			| (((Instruction)(b)) << POS_B) \
			| (((Instruction)(c)) << POS_C))

#define OS_OPCODE_ABx(o,a,bc)	((((Instruction)(o)) << POS_OP) \
			| (((Instruction)(a)) << POS_A) \
			| (((Instruction)(bc)) << POS_Bx))

#define OS_OPCODE_Ax(o,a)		((((Instruction)(o)) << POS_OP) \
			| (((Instruction)(a)) << POS_Ax)) 

// =====================================================================
// =====================================================================
// =====================================================================

#if defined __GNUC__ || defined IW_SDK

int OS_VSNPRINTF(OS_CHAR * str, size_t size, const OS_CHAR *format, va_list va)
{
	return vsnprintf(str, size, format, va);
}

#else

int OS_VSNPRINTF(OS_CHAR * str, size_t size, const OS_CHAR *format, va_list va)
{
	return vsnprintf_s(str, size, size/sizeof(OS_CHAR), format, va);
}

#endif

int OS_SNPRINTF(OS_CHAR * str, size_t size, const OS_CHAR *format, ...)
{

	va_list va;
	va_start(va, format);
	int ret = OS_VSNPRINTF(str, size, format, va);
	va_end(va);
	return ret;
}

static bool OS_ISNAN(float a)
{
	volatile float b = a;
	return b != b;
}

static bool OS_ISNAN(double a)
{
	volatile double b = a;
	return b != b;
}

#include <float.h>
#include <limits.h>

template <class T> T OS_getMaxValue();
template <> double OS_getMaxValue<double>(){ return DBL_MAX; }
template <> float OS_getMaxValue<float>(){ return FLT_MAX; }
template <> int OS_getMaxValue<int>(){ return INT_MAX; }

#define OS_MAX_NUMBER OS_getMaxValue<OS_NUMBER>()

#define CURRENT_BYTE_ORDER       (*(OS_INT32*)"\x01\x02\x03\x04")
#define LITTLE_ENDIAN_BYTE_ORDER 0x04030201
#define BIG_ENDIAN_BYTE_ORDER    0x01020304
#define PDP_ENDIAN_BYTE_ORDER    0x02010403

#define IS_LITTLE_ENDIAN (CURRENT_BYTE_ORDER == LITTLE_ENDIAN_BYTE_ORDER)
#define IS_BIG_ENDIAN    (CURRENT_BYTE_ORDER == BIG_ENDIAN_BYTE_ORDER)
#define IS_PDP_ENDIAN    (CURRENT_BYTE_ORDER == PDP_ENDIAN_BYTE_ORDER)

static inline OS_BYTE toLittleEndianByteOrder(OS_BYTE val)
{
	OS_ASSERT(sizeof(val) == sizeof(OS_BYTE)*1);
	return val;
}

static inline OS_INT8 toLittleEndianByteOrder(OS_INT8 val)
{
	OS_ASSERT(sizeof(val) == sizeof(OS_BYTE)*1);
	return val;
}

static inline OS_U16 toLittleEndianByteOrder(OS_U16 val)
{
	OS_ASSERT(sizeof(val) == sizeof(OS_BYTE)*2);
	if(IS_LITTLE_ENDIAN){
		return val;
	}
	OS_U16 r;
	((OS_BYTE*)&r)[0] = ((OS_BYTE*)&val)[1];
	((OS_BYTE*)&r)[1] = ((OS_BYTE*)&val)[0];
	return r;
}

static inline OS_INT16 toLittleEndianByteOrder(OS_INT16 val)
{
	OS_ASSERT(sizeof(val) == sizeof(OS_BYTE)*2);
	if(IS_LITTLE_ENDIAN){
		return val;
	}
	OS_INT16 r;
	((OS_BYTE*)&r)[0] = ((OS_BYTE*)&val)[1];
	((OS_BYTE*)&r)[1] = ((OS_BYTE*)&val)[0];
	return r;
}

static inline OS_INT32 toLittleEndianByteOrder(OS_INT32 val)
{
	OS_ASSERT(sizeof(val) == sizeof(OS_BYTE)*4);
	if(IS_LITTLE_ENDIAN){
		return val;
	}
	OS_INT32 r;
	((OS_BYTE*)&r)[0] = ((OS_BYTE*)&val)[3];
	((OS_BYTE*)&r)[1] = ((OS_BYTE*)&val)[2];
	((OS_BYTE*)&r)[2] = ((OS_BYTE*)&val)[1];
	((OS_BYTE*)&r)[3] = ((OS_BYTE*)&val)[0];
	return r;
}

static inline OS_INT64 toLittleEndianByteOrder(OS_INT64 val)
{
	OS_ASSERT(sizeof(val) == sizeof(OS_BYTE)*8);
	if(IS_LITTLE_ENDIAN){
		return val;
	}
	OS_INT64 r;
	((OS_BYTE*)&r)[0] = ((OS_BYTE*)&val)[7];
	((OS_BYTE*)&r)[1] = ((OS_BYTE*)&val)[6];
	((OS_BYTE*)&r)[2] = ((OS_BYTE*)&val)[5];
	((OS_BYTE*)&r)[3] = ((OS_BYTE*)&val)[4];
	((OS_BYTE*)&r)[4] = ((OS_BYTE*)&val)[3];
	((OS_BYTE*)&r)[5] = ((OS_BYTE*)&val)[2];
	((OS_BYTE*)&r)[6] = ((OS_BYTE*)&val)[1];
	((OS_BYTE*)&r)[7] = ((OS_BYTE*)&val)[0];
	return r;
}

static inline float toLittleEndianByteOrder(float val)
{
	OS_ASSERT(sizeof(val) == sizeof(OS_BYTE)*4);
	if(IS_LITTLE_ENDIAN){
		return val;
	}
	float r;
	((OS_BYTE*)&r)[0] = ((OS_BYTE*)&val)[3];
	((OS_BYTE*)&r)[1] = ((OS_BYTE*)&val)[2];
	((OS_BYTE*)&r)[2] = ((OS_BYTE*)&val)[1];
	((OS_BYTE*)&r)[3] = ((OS_BYTE*)&val)[0];
	return r;
}

static inline double toLittleEndianByteOrder(double val)
{
	OS_ASSERT(sizeof(val) == sizeof(OS_BYTE)*8);
	if(IS_LITTLE_ENDIAN){
		return val;
	}
	double r;
	((OS_BYTE*)&r)[0] = ((OS_BYTE*)&val)[7];
	((OS_BYTE*)&r)[1] = ((OS_BYTE*)&val)[6];
	((OS_BYTE*)&r)[2] = ((OS_BYTE*)&val)[5];
	((OS_BYTE*)&r)[3] = ((OS_BYTE*)&val)[4];
	((OS_BYTE*)&r)[4] = ((OS_BYTE*)&val)[3];
	((OS_BYTE*)&r)[5] = ((OS_BYTE*)&val)[2];
	((OS_BYTE*)&r)[6] = ((OS_BYTE*)&val)[1];
	((OS_BYTE*)&r)[7] = ((OS_BYTE*)&val)[0];
	return r;
}

#define fromLittleEndianByteOrder toLittleEndianByteOrder

static const OS_INT32 nan_data = 0x7fc00000;
static const float nan_float = fromLittleEndianByteOrder(*(float*)&nan_data);

static inline void parseSpaces(const OS_CHAR *& str)
{
	while(*str && OS_IS_SPACE(*str))
		str++;
}

template <class T>
static bool parseSimpleHex(const OS_CHAR *& p_str, T& p_val)
{
	T val = 0, prev_val = 0;
	const OS_CHAR * str = p_str;
	const OS_CHAR * start = str;
	for(;; str++){
		if(*str >= OS_TEXT('0') && *str <= OS_TEXT('9')){
			val = (val << 4) + (T)(*str - OS_TEXT('0'));
		}else if(*str >= OS_TEXT('a') && *str <= OS_TEXT('f')){
			val = (val << 4) + 10 + (T)(*str - OS_TEXT('a'));
		}else if(*str >= OS_TEXT('A') && *str <= OS_TEXT('F')){
			val = (val << 4) + 10 + (T)(*str - OS_TEXT('A'));
		}else{
			break;
		}
		if(prev_val > val){
			p_str = start;
			p_val = 0;
			return false;
		}
		prev_val = val;
	}
	p_val = val;
	p_str = str;
	return str > start;
}

template <class T>
static bool parseSimpleBin(const OS_CHAR *& p_str, T& p_val)
{
	T val = 0, prev_val = 0;
	const OS_CHAR * str = p_str;
	const OS_CHAR * start = str;
	for(; *str >= OS_TEXT('0') && *str <= OS_TEXT('1'); str++){
		val = (val << 1) + (T)(*str - OS_TEXT('0'));
		if(prev_val > val){
			p_str = start;
			p_val = 0;
			return false;
		}
		prev_val = val;
	}
	p_val = val;
	p_str = str;
	return str > start;
}

template <class T>
static bool parseSimpleOctal(const OS_CHAR *& p_str, T& p_val)
{
	T val = 0, prev_val = 0;
	const OS_CHAR * str = p_str;
	const OS_CHAR * start = str;
	for(; *str >= OS_TEXT('0') && *str <= OS_TEXT('7'); str++)
	{
		val = (val << 3) + (T)(*str - OS_TEXT('0'));
		if(prev_val > val){
			p_str = start;
			p_val = 0;
			return false;
		}
		prev_val = val;
	}
	p_val = val;
	p_str = str;
	return str > start;
}

template <class T>
static bool parseSimpleDec(const OS_CHAR *& p_str, T& p_val)
{
	T val = 0, prev_val = 0;
	const OS_CHAR * str = p_str;
	const OS_CHAR * start = str;
	for(; *str >= OS_TEXT('0') && *str <= OS_TEXT('9'); str++){
		val = val * 10 + (T)(*str - OS_TEXT('0'));
		if(prev_val > val){
			p_str = start;
			p_val = 0;
			return false;
		}
		prev_val = val;
	}
	p_val = val;
	p_str = str;
	return str > start;
}

template <class T>
static bool parseSimpleFloat(const OS_CHAR *& p_str, T& p_val)
{
	T val = 0;
	const OS_CHAR * str = p_str;
	const OS_CHAR * start = str;
	for(; *str >= OS_TEXT('0') && *str <= OS_TEXT('9'); str++){
		val = val * 10 + (*str - OS_TEXT('0'));
	}
	p_val = val;
	p_str = str;
	return str > start;
}

bool OS::Utils::parseFloat(const OS_CHAR *& str, OS_FLOAT& result)
{
	const OS_CHAR * start_str = str;
	int sign = 1;
	if(*str == OS_TEXT('-')){
		str++;
		start_str++;
		sign = -1;
	}else if(*str == OS_TEXT('+')){
		str++;
		start_str++;
	}

	if(str[0] == OS_TEXT('0') && str[1] != OS_TEXT('.')){
		bool is_valid, is_octal = false;
		OS_INT int_val;
		if(str[1] == OS_TEXT('x') || str[1] == OS_TEXT('X')){ // parse hex
			str += 2;
			is_valid = parseSimpleHex(str, int_val);
		}else if(str[1] == OS_TEXT('b') || str[1] == OS_TEXT('B')){ // parse hex
			str += 2;
			is_valid = parseSimpleBin(str, int_val);
		}else{ // parse octal
			is_octal = true;
			is_valid = parseSimpleOctal(str, int_val);
		}
		if(!is_valid || (start_str+1 == str && !is_octal)){
			result = 0;
			return false;
		}
		if((OS_INT)(OS_FLOAT)int_val != int_val){
			result = 0;
			return false;
		}
		result = (OS_FLOAT)int_val;
		return true;
	}

	OS_FLOAT float_val;
	if(!parseSimpleFloat(str, float_val)){
		result = 0;
		return false;
	}

	if(*str == OS_TEXT('.')){ // parse float
		// parse 1.#INF ...
		if(sign == 1 && start_str+1 == str && *start_str == OS_TEXT('1') && str[1] == OS_TEXT('#')){
			const OS_CHAR * spec[] = {OS_TEXT("INF"), OS_TEXT("IND"), OS_TEXT("QNAN"), NULL};
			int i = 0;
			for(; spec[i]; i++){
				if(OS_STRCMP(str, spec[i]) != 0)
					continue;

				size_t specLen = OS_STRLEN(spec[i]);
				str += specLen;
				if(!*str || OS_IS_SPACE(*str) || OS_STRCHR(OS_TEXT("!@#$%^&*()-+={}[]\\|;:'\",<.>/?`~"), *str)){
					OS_INT32 spec_val;
					switch(i){
					case 0:
						spec_val = 0x7f800000;
						break;

					case 1:
						spec_val = 0xffc00000;
						break;

					default:
						OS_ASSERT(false);
						// no break

					case 2:
						spec_val = 0x7fc00000;
						break;
					}
					result = (OS_FLOAT)fromLittleEndianByteOrder(*(float*)&spec_val);
					return true;
				}            
			}
			result = 0;
			return false;
		}

		OS_FLOAT m = (OS_FLOAT)0.1;
		for(str++; *str >= OS_TEXT('0') && *str <= OS_TEXT('9'); str++, m *= (OS_FLOAT)0.1){
			float_val += (OS_FLOAT)(*str - OS_TEXT('0')) * m;
		}
		if(start_str == str){
			result = 0;
			return false;
		}
		if(*str == OS_TEXT('e') || *str == OS_TEXT('E')){
			str++;
			bool div = false; // + for default
			if(*str == OS_TEXT('-')){
				div = true;
				str++;
			}else if(*str == OS_TEXT('+')){
				// div = false;
				str++;
			}
			int pow;
			if(!parseSimpleDec(str, pow)){
				result = 0;
				return false;
			}
			m = 1.0f;
			for(int i = 0; i < pow; i++){
				m *= 10.0f;
			}
			if(div){
				float_val /= m;
			}else{
				float_val *= m;
			}
		}
		result = sign > 0 ? float_val : -float_val;
		return true;
	}
	if(start_str == str){
		result = 0;
		return false;
	}
	result = sign > 0 ? float_val : -float_val;
	return true;
}

OS_CHAR * OS::Utils::numToStr(OS_CHAR * dst, OS_INT32 a)
{
	OS_SNPRINTF(dst, sizeof(OS_CHAR)*63, OS_TEXT("%i"), a);
	return dst;
}

OS_CHAR * OS::Utils::numToStr(OS_CHAR * dst, OS_INT64 a)
{
	OS_SNPRINTF(dst, sizeof(OS_CHAR)*63, OS_TEXT("%li"), (long int)a);
	return dst;
}

OS_CHAR * OS::Utils::numToStr(OS_CHAR * dst, float a, int precision)
{
	return numToStr(dst, (double)a, precision);
}

OS_CHAR * OS::Utils::numToStr(OS_CHAR * dst, double a, int precision)
{
	// OS_CHAR buf[128];
	if(precision <= 0) {
		if(precision < 0) {
			OS_FLOAT p = 10.0f;
			for(int i = -precision-1; i > 0; i--){
				p *= 10.0f;
			}
			a = ::floor(a / p + 0.5f) * p;
		}
		OS_SNPRINTF(dst, sizeof(OS_CHAR)*127, OS_TEXT("%.f"), a);
		return dst;
	}
	if(precision == OS_AUTO_PRECISION){
		/* %G already handles removing trailing zeros from the fractional part, yay */ 
		OS_SNPRINTF(dst, sizeof(OS_CHAR)*127, OS_TEXT("%.*G"), 17, a);
		return dst;
	}
	int n = OS_SNPRINTF(dst, sizeof(OS_CHAR)*127, OS_TEXT("%.*f"), precision, a);
	// OS_SNPRINTF(buf, sizeof(buf)-sizeof(OS_CHAR), OS_TEXT("%%.%df"), precision);
	// int n = OS_SNPRINTF(dst, sizeof(buf)-sizeof(OS_CHAR), buf, a);
	OS_ASSERT(n >= 1 && !OS_STRSTR(dst, OS_TEXT(".")) || dst[n-1] != '0');
	/* if(n > 0 && dst[n-1] == '0'){
		do{ dst[--n] = (OS_CHAR)0; }while(n > 0 && dst[n-1] == '0');
		if(n > 0 && dst[n-1] == '.') dst[--n] = (OS_CHAR)0;
	} */
	return dst;
}

OS_INT OS::Utils::strToInt(const OS_CHAR * str)
{
	return (OS_INT)strToFloat(str);
}

OS_FLOAT OS::Utils::strToFloat(const OS_CHAR* str)
{
	OS_FLOAT fval;
	if(parseFloat(str, fval) && (!*str || (*str==OS_TEXT('f') && !str[1]))){
		return fval;
	}
	return 0;
}

#define OS_STR_HASH_START_VALUE 5381
#define OS_ADD_STR_HASH_VALUE hash = ((hash << 5) + hash) + *buf

#define OS_STR_HASH_LIMIT_SHIFT 5

int OS::Utils::keyToHash(const void * p_buf, int size)
{
	OS_ASSERT(size >= 0);
	int step = (size >> OS_STR_HASH_LIMIT_SHIFT) + 1;
	const OS_BYTE * buf = (const OS_BYTE*)p_buf;
	const OS_BYTE * end = buf + size;
	int hash = OS_STR_HASH_START_VALUE;
	for(; buf < end; buf += step){
		OS_ADD_STR_HASH_VALUE;
	}
	return hash;
}

int OS::Utils::keyToHash(const void * buf1, int size1, const void * buf2, int size2)
{
	OS_ASSERT(size1 >= 0 && size2 >= 0);
	if(size2 > 0){
		int size = size1 + size2;
		int step = (size >> OS_STR_HASH_LIMIT_SHIFT) + 1;
		const OS_BYTE * buf = (const OS_BYTE*)buf1;
		const OS_BYTE * end = buf + size1;
		int hash = OS_STR_HASH_START_VALUE;
		for(; buf < end; buf += step){
			OS_ADD_STR_HASH_VALUE;
		}
		buf = (const OS_BYTE*)buf2 + (buf - end);
		end = buf + size2;
		for(; buf < end; buf += step){
			OS_ADD_STR_HASH_VALUE;
		}
		return hash;
	}
	return keyToHash(buf1, size1);
}

int OS::Utils::cmp(const void * buf1, int len1, const void * buf2, int len2)
{
	int len = len1 < len2 ? len1 : len2;
	int cmp = OS_MEMCMP(buf1, buf2, len);
	return cmp ? cmp : len1 - len2;
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::Core::String::String(OS * os)
{
	string = os->core->newStringValue((void*)NULL, 0);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(GCStringValue * s)
{
	string = s;
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(const String& s)
{
	string = s.string;
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, const String& a, const String& b)
{
	string = os->core->newStringValue(a, b);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, const OS_CHAR * str)
{
	string = os->core->newStringValue(str);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, const OS_CHAR * str, int len)
{
	string = os->core->newStringValue(str, len);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, const OS_CHAR * str, int len, const OS_CHAR * str2, int len2)
{
	string = os->core->newStringValue(str, len, str2, len2);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, const OS_CHAR * str, int len, bool trim_left, bool trim_right)
{
	string = os->core->newStringValue(str, len, trim_left, trim_right);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, const void * buf, int size)
{
	string = os->core->newStringValue(buf, size);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, const void * buf1, int size1, const void * buf2, int size2)
{
	string = os->core->newStringValue(buf1, size1, buf2, size2);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, const void * buf1, int size1, const void * buf2, int size2, const void * buf3, int size3)
{
	string = os->core->newStringValue(buf1, size1, buf2, size2, buf3, size3);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, OS_INT value)
{
	string = os->core->newStringValue(value);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::String(OS * os, OS_FLOAT value, int precision)
{
	string = os->core->newStringValue(value, precision);
	string->external_ref_count++;
#ifdef OS_DEBUG
	this->str = string->toChar();
#endif
}

OS::Core::String::~String()
{
	if(string){ // can be cleared by OS::~String
		OS_ASSERT(string->external_ref_count > 0);
		string->external_ref_count--;
		if(string->gc_color == GC_WHITE){
			string->gc_color = GC_BLACK;
		}
	}
}

struct OS_VaListDtor
{
	va_list * va;

	OS_VaListDtor(va_list * p_va){ va = p_va; }
	~OS_VaListDtor(){ va_end(*va); }
};

OS::Core::String OS::Core::String::format(OS * allocator, int temp_buf_len, const OS_CHAR * fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	OS_VaListDtor va_dtor(&va);
	return String(allocator->core->newStringValueVa(temp_buf_len, fmt, va));
}

OS::Core::String OS::Core::String::formatVa(OS * allocator, int temp_buf_len, const OS_CHAR * fmt, va_list va)
{
	return String(allocator->core->newStringValueVa(temp_buf_len, fmt, va));
}

OS::Core::String OS::Core::String::format(OS * allocator, const OS_CHAR * fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	OS_VaListDtor va_dtor(&va);
	return String(allocator->core->newStringValueVa(OS_DEF_FMT_BUF_LEN, fmt, va));
}

OS::Core::String OS::Core::String::formatVa(OS * allocator, const OS_CHAR * fmt, va_list va)
{
	return String(allocator->core->newStringValueVa(OS_DEF_FMT_BUF_LEN, fmt, va));
}

OS::Core::String& OS::Core::String::operator=(const String& b)
{
	if(string != b.string){
		OS_ASSERT(string->external_ref_count > 0);
		string->external_ref_count--;
		if(string->gc_color == GC_WHITE){
			string->gc_color = GC_BLACK;
		}
		string = b.string;
		string->external_ref_count++;
#ifdef OS_DEBUG
		this->str = string->toChar();
#endif
	}
	return *this;
}

bool OS::Core::String::operator==(const String& b) const
{
	return string == b.string;
}

bool OS::Core::String::operator==(const OS_CHAR * b) const
{
	return cmp(b) == 0;
}

bool OS::Core::String::operator==(GCStringValue * b) const
{
	return string == b;
}

bool OS::Core::String::operator!=(const String& b) const
{
	return string != b.string;
}

bool OS::Core::String::operator!=(const OS_CHAR * b) const
{
	return cmp(b) != 0;
}

bool OS::Core::String::operator!=(GCStringValue * b) const
{
	return string != b;
}

bool OS::Core::String::operator<=(const String& b) const
{
	return cmp(b) <= 0;
}

bool OS::Core::String::operator<=(const OS_CHAR * b) const
{
	return cmp(b) <= 0;
}

bool OS::Core::String::operator<(const String& b) const
{
	return cmp(b) < 0;
}

bool OS::Core::String::operator<(const OS_CHAR * b) const
{
	return cmp(b) < 0;
}

bool OS::Core::String::operator>=(const String& b) const
{
	return cmp(b) >= 0;
}

bool OS::Core::String::operator>=(const OS_CHAR * b) const
{
	return cmp(b) >= 0;
}

bool OS::Core::String::operator>(const String& b) const
{
	return cmp(b) > 0;
}

bool OS::Core::String::operator>(const OS_CHAR * b) const
{
	return cmp(b) > 0;
}

int OS::Core::String::cmp(const String& b) const
{
	if(string == b.string){
		return 0;
	}
	return Utils::cmp(string->toChar(), string->data_size, b.string->toChar(), b.string->data_size);
}

int OS::Core::String::cmp(const OS_CHAR * b) const
{
	return Utils::cmp(string->toChar(), string->data_size, b, OS_STRLEN(b));
}

int OS::Core::String::getHash() const
{
	return string->hash;
}

OS_NUMBER OS::Core::String::toNumber() const
{
	return string->toNumber();
}

// =====================================================================

OS::Core::StringBuffer::StringBuffer(OS * p_allocator)
{
	allocator = p_allocator;
}

OS::Core::StringBuffer::~StringBuffer()
{
	allocator->vectorClear(*this);
}

OS::Core::StringBuffer& OS::Core::StringBuffer::append(OS_CHAR c)
{
	allocator->vectorAddItem(*this, c OS_DBG_FILEPOS);
	return *this;
}

OS::Core::StringBuffer& OS::Core::StringBuffer::append(const OS_CHAR * str)
{
	return append(str, OS_STRLEN(str));
}

OS::Core::StringBuffer& OS::Core::StringBuffer::append(const OS_CHAR * str, int len)
{
	allocator->vectorReserveCapacity(*this, count + len OS_DBG_FILEPOS);
	OS_MEMCPY(buf + count, str, len * sizeof(OS_CHAR));
	count += len;
	return *this;
}

OS::Core::StringBuffer& OS::Core::StringBuffer::append(const String& str)
{
	return append(str.toChar(), str.getLen());
}

OS::Core::StringBuffer& OS::Core::StringBuffer::append(const StringBuffer& buf)
{
	return append(buf.buf, buf.count);
}

OS::Core::StringBuffer& OS::Core::StringBuffer::operator+=(const String& str)
{
	return append(str);
}

OS::Core::StringBuffer& OS::Core::StringBuffer::operator+=(const OS_CHAR * str)
{
	return append(str);
}

OS::Core::StringBuffer::operator OS::Core::String() const
{
	return toString();
}

OS::Core::String OS::Core::StringBuffer::toString() const
{
	return String(allocator, buf, count);
}

OS::Core::GCStringValue * OS::Core::StringBuffer::toGCStringValue() const
{
	return allocator->core->newStringValue(buf, count);
}

// =====================================================================

OS::String::String(OS * allocator): super(allocator)
{
	this->allocator = allocator->retain();
}

OS::String::String(const String& str): super(str)
{
	allocator = str.allocator->retain();
}

OS::String::String(OS * allocator, const Core::String& str): super(str)
{
	this->allocator = allocator->retain();
}

OS::String::String(OS * allocator, const OS_CHAR * str): super(allocator, str)
{
	this->allocator = allocator->retain();
}

OS::String::String(OS * allocator, const OS_CHAR * str1, int len1, const OS_CHAR * str2, int len2): super(allocator, str1, len1, str2, len2)
{
	this->allocator = allocator->retain();
}

OS::String::String(OS * allocator, const OS_CHAR * str, int len): super(allocator, str, len)
{
	this->allocator = allocator->retain();
}

OS::String::String(OS * allocator, const OS_CHAR * str, int len, bool trim_left, bool trim_right): super(allocator, str, len, trim_left, trim_right)
{
	this->allocator = allocator->retain();
}

OS::String::String(OS * allocator, const void * buf, int size): super(allocator, buf, size)
{
	this->allocator = allocator->retain();
}

OS::String::String(OS * allocator, const void * buf1, int size1, const void * buf2, int size2): super(allocator, buf1, size1, buf2, size2)
{
	this->allocator = allocator->retain();
}

/*
OS::String::String(OS * allocator, const void * buf1, int size1, const void * buf2, int size2, const void * buf3, int size3): super(allocator, buf1, size1, buf2, size2, buf3, size3)
{
this->allocator = allocator->retain();
}
*/

OS::String::String(OS * allocator, OS_INT value): super(allocator, value)
{
	this->allocator = allocator->retain();
}

OS::String::String(OS * allocator, OS_FLOAT value, int precision): super(allocator, value, precision)
{
	this->allocator = allocator->retain();
}

OS::String::~String()
{
	OS_ASSERT(string->external_ref_count > 0);
	string->external_ref_count--;
	if(string->gc_color == Core::GC_WHITE){
		string->gc_color = Core::GC_BLACK;
	}
	string = NULL;
	allocator->release();
}

OS::String& OS::String::operator=(const Core::String& str)
{
	if(string != str.string){
		OS_ASSERT(string->external_ref_count > 0);
		string->external_ref_count--;
		if(string->gc_color == Core::GC_WHITE){
			string->gc_color = Core::GC_BLACK;
		}
		string = str.string;
		string->external_ref_count++;
#ifdef OS_DEBUG
		this->str = string->toChar();
#endif
	}
	return *this;
}

OS::String& OS::String::operator=(const String& str)
{
	OS_ASSERT(allocator == str.allocator);
	if(string != str.string){
		OS_ASSERT(string->external_ref_count > 0);
		string->external_ref_count--;
		if(string->gc_color == Core::GC_WHITE){
			string->gc_color = Core::GC_BLACK;
		}
		string = str.string;
		string->external_ref_count++;
#ifdef OS_DEBUG
		this->str = string->toChar();
#endif
	}
	return *this;
}

OS::String& OS::String::operator+=(const String& str)
{
	return *this = allocator->core->newStringValue(*this, str);
}

OS::String& OS::String::operator+=(const OS_CHAR * str)
{
	return *this = allocator->core->newStringValue(toChar(), getDataSize(), str, OS_STRLEN(str)*sizeof(OS_CHAR));
}

OS::String OS::String::operator+(const String& str) const
{
	return String(allocator, allocator->core->newStringValue(*this, str));
}

OS::String OS::String::operator+(const OS_CHAR * str) const
{
	return String(allocator, allocator->core->newStringValue(toChar(), getDataSize(), str, OS_STRLEN(str)*sizeof(OS_CHAR)));
}

OS::String OS::String::trim(bool trim_left, bool trim_right) const
{
	return String(allocator, allocator->core->newStringValue(*this, trim_left, trim_right));
}

// =====================================================================
// =====================================================================
// =====================================================================

const OS_CHAR * OS::Core::Tokenizer::getTokenTypeName(TokenType token_type)
{
	switch(token_type){
	case NOTHING: return OS_TEXT("NOTHING");

	case BEGIN_CODE_BLOCK:    return OS_TEXT("BEGIN_CODE_BLOCK");
	case END_CODE_BLOCK:      return OS_TEXT("END_CODE_BLOCK");

	case BEGIN_BRACKET_BLOCK: return OS_TEXT("BEGIN_BRACKET_BLOCK");
	case END_BRACKET_BLOCK:   return OS_TEXT("END_BRACKET_BLOCK");

	case BEGIN_ARRAY_BLOCK:   return OS_TEXT("BEGIN_ARRAY_BLOCK");
	case END_ARRAY_BLOCK:     return OS_TEXT("END_ARRAY_BLOCK");

	case CODE_SEPARATOR:      return OS_TEXT("CODE_SEPARATOR");
	case PARAM_SEPARATOR:     return OS_TEXT("PARAM_SEPARATOR");

	case COMMENT_LINE:        return OS_TEXT("COMMENT_LINE");
	case COMMENT_MULTI_LINE:  return OS_TEXT("COMMENT_MULTI_LINE");

	case NAME:      return OS_TEXT("NAME");
		// case DOT_NAME:  return OS_TEXT("DOT_NAME");
		// case IDENTIFER:  return OS_TEXT("IDENTIFER");
		// case DOT_IDENTIFER:  return OS_TEXT("DOT_IDENTIFER");
	case STRING:    return OS_TEXT("STRING");

	case NUMBER:   return OS_TEXT("NUMBER");
		// case NUM_VECTOR_3:  return OS_TEXT("NUM_VECTOR_3");
		// case NUM_VECTOR_4:  return OS_TEXT("NUM_VECTOR_4");

	case OPERATOR:        return OS_TEXT("OPERATOR");
	case BINARY_OPERATOR: return OS_TEXT("BINARY_OPERATOR");

	case OPERATOR_INDIRECT: return OS_TEXT("OPERATOR_INDIRECT");
	case OPERATOR_CONCAT:  return OS_TEXT("OPERATOR_CONCAT");

	case OPERATOR_LOGIC_AND:  return OS_TEXT("OPERATOR_LOGIC_AND");
	case OPERATOR_LOGIC_OR:   return OS_TEXT("OPERATOR_LOGIC_OR");
	case OPERATOR_LOGIC_PTR_EQ:   return OS_TEXT("OPERATOR_LOGIC_PTR_EQ");
	case OPERATOR_LOGIC_PTR_NE:   return OS_TEXT("OPERATOR_LOGIC_PTR_NE");
	case OPERATOR_LOGIC_EQ:   return OS_TEXT("OPERATOR_LOGIC_EQ");
	case OPERATOR_LOGIC_NE:   return OS_TEXT("OPERATOR_LOGIC_NE");
	case OPERATOR_LOGIC_GE:   return OS_TEXT("OPERATOR_LOGIC_GE");
	case OPERATOR_LOGIC_LE:   return OS_TEXT("OPERATOR_LOGIC_LE");
	case OPERATOR_LOGIC_GREATER:  return OS_TEXT("OPERATOR_LOGIC_GREATER");
	case OPERATOR_LOGIC_LESS: return OS_TEXT("OPERATOR_LOGIC_LESS");
	case OPERATOR_LOGIC_NOT:  return OS_TEXT("OPERATOR_LOGIC_NOT");

	case OPERATOR_INC:  return OS_TEXT("OPERATOR_INC");
	case OPERATOR_DEC:  return OS_TEXT("OPERATOR_DEC");

	case OPERATOR_QUESTION: return OS_TEXT("OPERATOR_QUESTION");
	case OPERATOR_COLON:    return OS_TEXT("OPERATOR_COLON");

	case OPERATOR_BIT_AND:  return OS_TEXT("OPERATOR_BIT_AND");
	case OPERATOR_BIT_OR:   return OS_TEXT("OPERATOR_BIT_OR");
	case OPERATOR_BIT_XOR:  return OS_TEXT("OPERATOR_BIT_XOR");
	case OPERATOR_BIT_NOT:  return OS_TEXT("OPERATOR_BIT_NOT");
	case OPERATOR_ADD:      return OS_TEXT("OPERATOR_ADD");
	case OPERATOR_SUB:      return OS_TEXT("OPERATOR_SUB");
	case OPERATOR_MUL:      return OS_TEXT("OPERATOR_MUL");
	case OPERATOR_DIV:      return OS_TEXT("OPERATOR_DIV");
	case OPERATOR_MOD:      return OS_TEXT("OPERATOR_MOD");
	case OPERATOR_LSHIFT:  return OS_TEXT("OPERATOR_LSHIFT");
	case OPERATOR_RSHIFT:  return OS_TEXT("OPERATOR_RSHIFT");
	case OPERATOR_POW:      return OS_TEXT("OPERATOR_POW");

	case OPERATOR_BIT_AND_ASSIGN: return OS_TEXT("OPERATOR_BIT_AND_ASSIGN");
	case OPERATOR_BIT_OR_ASSIGN:  return OS_TEXT("OPERATOR_BIT_OR_ASSIGN");
	case OPERATOR_BIT_XOR_ASSIGN: return OS_TEXT("OPERATOR_BIT_XOR_ASSIGN");
	case OPERATOR_BIT_NOT_ASSIGN: return OS_TEXT("OPERATOR_BIT_NOT_ASSIGN");
	case OPERATOR_ADD_ASSIGN:     return OS_TEXT("OPERATOR_ADD_ASSIGN");
	case OPERATOR_SUB_ASSIGN:     return OS_TEXT("OPERATOR_SUB_ASSIGN");
	case OPERATOR_MUL_ASSIGN:     return OS_TEXT("OPERATOR_MUL_ASSIGN");
	case OPERATOR_DIV_ASSIGN:     return OS_TEXT("OPERATOR_DIV_ASSIGN");
	case OPERATOR_MOD_ASSIGN:     return OS_TEXT("OPERATOR_MOD_ASSIGN");
	case OPERATOR_LSHIFT_ASSIGN: return OS_TEXT("OPERATOR_LSHIFT_ASSIGN");
	case OPERATOR_RSHIFT_ASSIGN: return OS_TEXT("OPERATOR_RSHIFT_ASSIGN");
	case OPERATOR_POW_ASSIGN:     return OS_TEXT("OPERATOR_POW_ASSIGN");

	case OPERATOR_ASSIGN: return OS_TEXT("OPERATOR_ASSIGN");

	case OPERATOR_RESERVED: return OS_TEXT("OPERATOR_RESERVED");

	case OPERATOR_END:  return OS_TEXT("OPERATOR_END");

		// case PRE_PROCESSOR: return OS_TEXT("PRE_PROCESSOR");

	case ERROR_TOKEN:   return OS_TEXT("ERROR_TOKEN");
	}
	return OS_TEXT("UNKNOWN_TOKENTYPE");
}

OS::Core::Tokenizer::TokenData::TokenData(TextData * p_text_data, const String& p_str, TokenType p_type, int p_line, int p_pos): str(p_str)
{
	text_data = p_text_data->retain();
	ref_count = 1;
	type = p_type;
	line = p_line;
	pos = p_pos;
	// vec3 = NULL;
}

OS * OS::Core::Tokenizer::TokenData::getAllocator() const
{
	return text_data->allocator;
}

OS::Core::Tokenizer::TokenData::~TokenData()
{
	OS_ASSERT(ref_count == 0);
	text_data->release();
}

OS::Core::Tokenizer::TokenData * OS::Core::Tokenizer::TokenData::retain()
{
	ref_count++;
	return this;
}

void OS::Core::Tokenizer::TokenData::release()
{
	if(--ref_count <= 0){
		OS_ASSERT(ref_count == 0);
		OS * allocator = getAllocator();
		this->~TokenData();
		allocator->free(this);
	}
}

OS_FLOAT OS::Core::Tokenizer::TokenData::getFloat() const
{
	return float_value;
}

bool OS::Core::Tokenizer::TokenData::isTypeOf(TokenType token_type) const
{
	if(type == token_type){
		return true;
	}
	if(token_type == OS::Core::Tokenizer::SEPARATOR){
		switch(type)
		{
		case OS::Core::Tokenizer::BEGIN_CODE_BLOCK:  // {
		case OS::Core::Tokenizer::END_CODE_BLOCK:    // }

		case OS::Core::Tokenizer::BEGIN_BRACKET_BLOCK:  // (
		case OS::Core::Tokenizer::END_BRACKET_BLOCK:    // )

		case OS::Core::Tokenizer::BEGIN_ARRAY_BLOCK:  // [
		case OS::Core::Tokenizer::END_ARRAY_BLOCK:    // ]

		case OS::Core::Tokenizer::CODE_SEPARATOR:     // ;
		case OS::Core::Tokenizer::PARAM_SEPARATOR:    // ,
			return true;
		}
		return false;
	}
	if(token_type == BINARY_OPERATOR){
		switch(type)
		{
		case OS::Core::Tokenizer::PARAM_SEPARATOR:
		case OS::Core::Tokenizer::OPERATOR_QUESTION:

		case OS::Core::Tokenizer::OPERATOR_INDIRECT:  // .
		case OS::Core::Tokenizer::OPERATOR_CONCAT:	// ..
		case OS::Core::Tokenizer::OPERATOR_IN:		// in
		case OS::Core::Tokenizer::OPERATOR_ISPROTOTYPEOF:
		case OS::Core::Tokenizer::OPERATOR_IS:

		case OS::Core::Tokenizer::OPERATOR_LOGIC_AND: // &&
		case OS::Core::Tokenizer::OPERATOR_LOGIC_OR:  // ||
		case OS::Core::Tokenizer::OPERATOR_LOGIC_PTR_EQ:  // ===
		case OS::Core::Tokenizer::OPERATOR_LOGIC_PTR_NE:  // !==
		case OS::Core::Tokenizer::OPERATOR_LOGIC_EQ:  // ==
		case OS::Core::Tokenizer::OPERATOR_LOGIC_NE:  // !=
		case OS::Core::Tokenizer::OPERATOR_LOGIC_GE:  // >=
		case OS::Core::Tokenizer::OPERATOR_LOGIC_LE:  // <=
		case OS::Core::Tokenizer::OPERATOR_LOGIC_GREATER: // >
		case OS::Core::Tokenizer::OPERATOR_LOGIC_LESS:    // <

		case OS::Core::Tokenizer::OPERATOR_BIT_AND: // &
		case OS::Core::Tokenizer::OPERATOR_BIT_OR:  // |
		case OS::Core::Tokenizer::OPERATOR_BIT_XOR: // ^
		case OS::Core::Tokenizer::OPERATOR_BIT_NOT: // ~
		case OS::Core::Tokenizer::OPERATOR_ADD: // +
		case OS::Core::Tokenizer::OPERATOR_SUB: // -
		case OS::Core::Tokenizer::OPERATOR_MUL: // *
		case OS::Core::Tokenizer::OPERATOR_DIV: // /
		case OS::Core::Tokenizer::OPERATOR_MOD: // %
		case OS::Core::Tokenizer::OPERATOR_LSHIFT: // <<
		case OS::Core::Tokenizer::OPERATOR_RSHIFT: // >>
		case OS::Core::Tokenizer::OPERATOR_POW: // **

		case OS::Core::Tokenizer::OPERATOR_BIT_AND_ASSIGN: // &=
		case OS::Core::Tokenizer::OPERATOR_BIT_OR_ASSIGN:  // |=
		case OS::Core::Tokenizer::OPERATOR_BIT_XOR_ASSIGN: // ^=
		case OS::Core::Tokenizer::OPERATOR_BIT_NOT_ASSIGN: // ~=
		case OS::Core::Tokenizer::OPERATOR_ADD_ASSIGN: // +=
		case OS::Core::Tokenizer::OPERATOR_SUB_ASSIGN: // -=
		case OS::Core::Tokenizer::OPERATOR_MUL_ASSIGN: // *=
		case OS::Core::Tokenizer::OPERATOR_DIV_ASSIGN: // /=
		case OS::Core::Tokenizer::OPERATOR_MOD_ASSIGN: // %=
		case OS::Core::Tokenizer::OPERATOR_LSHIFT_ASSIGN: // <<=
		case OS::Core::Tokenizer::OPERATOR_RSHIFT_ASSIGN: // >>=
		case OS::Core::Tokenizer::OPERATOR_POW_ASSIGN: // **=

		case OS::Core::Tokenizer::OPERATOR_ASSIGN: // =
			return true;
		}
		return false;
	}
	return false;
}


bool OS::Core::Tokenizer::operator_initialized = false;
OS::Core::Tokenizer::OperatorDesc OS::Core::Tokenizer::operator_desc[] = 
{
	{ OPERATOR_INDIRECT, OS_TEXT(".") },
	{ OPERATOR_CONCAT, OS_TEXT("..") },
	{ REST_ARGUMENTS, OS_TEXT("...") },

	{ OPERATOR_RESERVED, OS_TEXT("->") },
	{ OPERATOR_RESERVED, OS_TEXT("::") },

	{ OPERATOR_LOGIC_AND, OS_TEXT("&&") },
	{ OPERATOR_LOGIC_OR,  OS_TEXT("||") },
	{ OPERATOR_LOGIC_PTR_EQ, OS_TEXT("===") },
	{ OPERATOR_LOGIC_PTR_NE, OS_TEXT("!==") },
	{ OPERATOR_LOGIC_EQ,  OS_TEXT("==") },
	{ OPERATOR_LOGIC_NE,  OS_TEXT("!=") },
	{ OPERATOR_LOGIC_GE,  OS_TEXT(">=") },
	{ OPERATOR_LOGIC_LE,  OS_TEXT("<=") },
	{ OPERATOR_LOGIC_GREATER, OS_TEXT(">") },
	{ OPERATOR_LOGIC_LESS,    OS_TEXT("<") },
	{ OPERATOR_LOGIC_NOT,     OS_TEXT("!") },

	{ OPERATOR_INC,     OS_TEXT("++") },
	{ OPERATOR_DEC,     OS_TEXT("--") },

	{ OPERATOR_QUESTION,  OS_TEXT("?") },
	{ OPERATOR_COLON,     OS_TEXT(":") },

	{ OPERATOR_LENGTH,    OS_TEXT("#") },

	{ OPERATOR_BIT_AND, OS_TEXT("&") },
	{ OPERATOR_BIT_OR,  OS_TEXT("|") },
	{ OPERATOR_BIT_XOR, OS_TEXT("^") },
	{ OPERATOR_BIT_NOT, OS_TEXT("~") },
	{ OPERATOR_CONCAT, OS_TEXT("..") },
	{ OPERATOR_ADD, OS_TEXT("+") },
	{ OPERATOR_SUB, OS_TEXT("-") },
	{ OPERATOR_MUL, OS_TEXT("*") },
	{ OPERATOR_DIV, OS_TEXT("/") },
	{ OPERATOR_MOD, OS_TEXT("%") },
	{ OPERATOR_LSHIFT, OS_TEXT("<<") },
	{ OPERATOR_RSHIFT, OS_TEXT(">>") },
	{ OPERATOR_POW, OS_TEXT("**") },

	{ OPERATOR_BIT_AND_ASSIGN, OS_TEXT("&=") },
	{ OPERATOR_BIT_OR_ASSIGN,  OS_TEXT("|=") },
	{ OPERATOR_BIT_XOR_ASSIGN, OS_TEXT("^=") },
	{ OPERATOR_BIT_NOT_ASSIGN, OS_TEXT("~=") },
	{ OPERATOR_ADD_ASSIGN, OS_TEXT("+=") },
	{ OPERATOR_SUB_ASSIGN, OS_TEXT("-=") },
	{ OPERATOR_MUL_ASSIGN, OS_TEXT("*=") },
	{ OPERATOR_DIV_ASSIGN, OS_TEXT("/=") },
	{ OPERATOR_MOD_ASSIGN, OS_TEXT("%=") },
	{ OPERATOR_LSHIFT_ASSIGN, OS_TEXT("<<=") },
	{ OPERATOR_RSHIFT_ASSIGN, OS_TEXT(">>=") },
	{ OPERATOR_POW_ASSIGN, OS_TEXT("**=") },

	{ OPERATOR_ASSIGN, OS_TEXT("=") },

	{ BEGIN_CODE_BLOCK, OS_TEXT("{") },
	{ END_CODE_BLOCK, OS_TEXT("}") },

	{ BEGIN_BRACKET_BLOCK, OS_TEXT("(") },
	{ END_BRACKET_BLOCK, OS_TEXT(")") },

	{ BEGIN_ARRAY_BLOCK, OS_TEXT("[") },
	{ END_ARRAY_BLOCK, OS_TEXT("]") },

	{ CODE_SEPARATOR, OS_TEXT(";") },
	{ PARAM_SEPARATOR, OS_TEXT(",") }
};

const int OS::Core::Tokenizer::operator_count = sizeof(operator_desc) / sizeof(operator_desc[0]);

int OS::Core::Tokenizer::compareOperatorDesc(const void * a, const void * b) 
{
	const OperatorDesc * op0 = (const OperatorDesc*)a;
	const OperatorDesc * op1 = (const OperatorDesc*)b;
	return (int)OS_STRLEN(op1->name) - (int)OS_STRLEN(op0->name);
}

void OS::Core::Tokenizer::initOperatorsTable()
{
	if(!operator_initialized){
		::qsort(operator_desc, operator_count, sizeof(operator_desc[0]), Tokenizer::compareOperatorDesc);
		operator_initialized = true;
	}
}

OS::Core::Tokenizer::TextData::TextData(OS * p_allocator): filename(p_allocator)
{
	allocator = p_allocator;
	ref_count = 1;
}

OS::Core::Tokenizer::TextData::~TextData()
{
	OS_ASSERT(!ref_count);
}

OS::Core::Tokenizer::TextData * OS::Core::Tokenizer::TextData::retain()
{
	ref_count++;
	return this;
}

void OS::Core::Tokenizer::TextData::release()
{
	if(--ref_count <= 0){
		OS_ASSERT(!ref_count);
		OS * allocator = this->allocator;
		allocator->vectorClear(lines);
		this->~TextData();
		allocator->free(this);
	}
}

OS::Core::Tokenizer::Tokenizer(OS * p_allocator)
{
	allocator = p_allocator;
	initOperatorsTable();
	settings.save_comments = false;
	error = ERROR_NOTHING;
	cur_line = 0;
	cur_pos = 0;

	text_data = new (allocator->malloc(sizeof(TextData) OS_DBG_FILEPOS)) TextData(allocator);
}

OS * OS::Core::Tokenizer::getAllocator()
{
	return allocator;
}

OS::Core::Tokenizer::~Tokenizer()
{
	OS * allocator = getAllocator();
	for(int i = 0; i < tokens.count; i++){
		TokenData * token = tokens[i];
		token->release();
	}
	allocator->vectorClear(tokens);
	// allocator->vectorClear(lines);
	text_data->release();
}

OS::Core::Tokenizer::TokenData * OS::Core::Tokenizer::removeToken(int i)
{
	TokenData * token = getToken(i);
	getAllocator()->vectorRemoveAtIndex(tokens, i);
	return token;
}

void OS::Core::Tokenizer::insertToken(int i, TokenData * token OS_DBG_FILEPOS_DECL)
{
	getAllocator()->vectorInsertAtIndex(tokens, i, token OS_DBG_FILEPOS_PARAM);
}

bool OS::Core::Tokenizer::parseText(const OS_CHAR * text, int len, const String& filename)
{
	OS_ASSERT(text_data->lines.count == 0);

	OS * allocator = getAllocator();

	// text_data->release();
	// text_data = new (allocator->malloc(sizeof(TextData))) TextData(allocator);
	text_data->filename = filename;

	const OS_CHAR * str = text;
	const OS_CHAR * str_end = str + len;
	while(str < str_end)
	{
#if 0
		const OS_CHAR * line_end = OS_STRCHR(str, OS_TEXT('\n'));
		if(line_end){
			allocator->vectorAddItem(text_data->lines, String(allocator, str, line_end - str, false, true) OS_DBG_FILEPOS);
			str = line_end+1;
		}else{
			allocator->vectorAddItem(text_data->lines, String(allocator, str, str_end - str, false, true) OS_DBG_FILEPOS);
			break;
		}
#else
		const OS_CHAR * line_end = str;
		for(; line_end < str_end && *line_end != OS_TEXT('\n'); line_end++);
		allocator->vectorAddItem(text_data->lines, String(allocator, str, line_end - str, false, true) OS_DBG_FILEPOS);
		str = line_end+1;
#endif
	}
	return parseLines();
}

void OS::Core::Tokenizer::TokenData::setFloat(OS_FLOAT value)
{
	float_value = value;
}

OS::Core::Tokenizer::TokenData * OS::Core::Tokenizer::addToken(const String& str, TokenType type, int line, int pos OS_DBG_FILEPOS_DECL)
{
	OS * allocator = getAllocator();
	TokenData * token = new (allocator->malloc(sizeof(TokenData) OS_DBG_FILEPOS_PARAM)) TokenData(text_data, str, type, line, pos);
	allocator->vectorAddItem(tokens, token OS_DBG_FILEPOS);
	return token;
}

static bool isValidCharAfterNumber(const OS_CHAR * str)
{
	return !*str || OS_IS_SPACE(*str) || OS_STRCHR(OS_TEXT("!@#$%^&*()-+={}[]\\|;:'\",<.>/?`~"), *str);
}

bool OS::Core::Tokenizer::parseFloat(const OS_CHAR *& str, OS_FLOAT& fval, bool parse_end_spaces)
{
	if(Utils::parseFloat(str, fval)){
		if(isValidCharAfterNumber(str)){
			if(parse_end_spaces){
				parseSpaces(str);
			}
			return true;
		}
		if(*str == OS_TEXT('f') && isValidCharAfterNumber(str+1)){
			str++;
			if(parse_end_spaces){
				parseSpaces(str);
			}
			return true;
		}
	}
	return false;
}

bool OS::Core::Tokenizer::parseLines()
{
	OS * allocator = getAllocator();
	cur_line = cur_pos = 0;
	for(; cur_line < text_data->lines.count; cur_line++){
		// parse line
		const OS_CHAR * line_start = text_data->lines[cur_line].toChar();
		const OS_CHAR * str = line_start;

		cur_pos = 0;
		for(;;){
			// skip spaces
			parseSpaces(str);
			if(!*str){
				break;
			}

			if(*str == OS_TEXT('"') || *str == OS_TEXT('\'')){ // begin string
				StringBuffer s(allocator);
				OS_CHAR closeChar = *str;
				const OS_CHAR * token_start = str;
				for(str++; *str && *str != closeChar;){
					OS_CHAR c = *str++;
					if(c == OS_TEXT('\\')){
						switch(*str){
						case OS_TEXT('r'): c = OS_TEXT('\r'); str++; break;
						case OS_TEXT('n'): c = OS_TEXT('\n'); str++; break;
						case OS_TEXT('t'): c = OS_TEXT('\t'); str++; break;
						case OS_TEXT('\"'): c = OS_TEXT('\"'); str++; break;
						case OS_TEXT('\''): c = OS_TEXT('\''); str++; break;
						case OS_TEXT('\\'): c = OS_TEXT('\\'); str++; break;
							//case OS_TEXT('x'): 
						default:
							{
								OS_INT val;
								int maxVal = sizeof(OS_CHAR) == 2 ? 0xFFFF : 0xFF;

								if(*str == OS_TEXT('x') || *str == OS_TEXT('X')){ // parse hex
									str++;
									if(!parseSimpleHex(str, val)){
										cur_pos = str - line_start;
										error = ERROR_CONST_STRING_ESCAPE_CHAR;
										return false;
									}
								}else if(*str == OS_TEXT('0')){ // octal
									if(!parseSimpleOctal(str, val)){
										cur_pos = str - line_start;
										error = ERROR_CONST_STRING_ESCAPE_CHAR;
										return false;
									}
								}else if(*str >= OS_TEXT('1') && *str <= OS_TEXT('9')){
									if(!parseSimpleDec(str, val)){
										cur_pos = str - line_start;
										error = ERROR_CONST_STRING_ESCAPE_CHAR;
										return false;
									}
								}else{
									val = c;
								}
								c = (OS_CHAR)(val <= maxVal ? val : maxVal);
							}
							break;
						}
					}
					s.append(c);
				}
				if(*str != closeChar){
					cur_pos = str - line_start;
					error = ERROR_CONST_STRING;
					return false;
				}
				str++;
				addToken(s, STRING, cur_line, token_start - line_start OS_DBG_FILEPOS);
				continue;
			}

			if(*str == OS_TEXT('/')){
				if(str[1] == OS_TEXT('/')){ // begin line comment
					if(settings.save_comments){
						addToken(String(allocator, str), COMMENT_LINE, cur_line, str - line_start OS_DBG_FILEPOS);
					}
					break;
				}
				if(str[1] == OS_TEXT('*')){ // begin multi line comment
					StringBuffer comment(allocator);
					comment.append(str, 2);
					int startLine = cur_line;
					int startPos = str - line_start;
					for(str += 2;;){
						const OS_CHAR * end = OS_STRSTR(str, OS_TEXT("*/"));
						if(end){
							if(settings.save_comments){
								comment.append(str, (int)(end+2 - str));
								addToken(comment, COMMENT_MULTI_LINE, startLine, startPos OS_DBG_FILEPOS);
							}
							str = end + 2;
							break;
						}
						if(cur_line >= text_data->lines.count){
							error = ERROR_MULTI_LINE_COMMENT;
							cur_pos = str - line_start;
							return false;
						}
						if(settings.save_comments){
							comment.append(str);
							comment.append(OS_TEXT("\n")); // OS_TEXT("\r\n"));
						}
						str = line_start = text_data->lines[++cur_line].toChar();
					}
					continue;
				}
			}

			if(*str == OS_TEXT('_') || *str == OS_TEXT('$') || *str == OS_TEXT('@') 
				|| (*str >= OS_TEXT('a') && *str <= OS_TEXT('z'))
				|| (*str >= OS_TEXT('A') && *str <= OS_TEXT('Z')) )
			{ // parse name
				const OS_CHAR * name_start = str;
				for(str++; *str; str++){
					if(*str == OS_TEXT('_') || *str == OS_TEXT('$') || *str == OS_TEXT('@')
						|| (*str >= OS_TEXT('a') && *str <= OS_TEXT('z'))
						|| (*str >= OS_TEXT('A') && *str <= OS_TEXT('Z'))
						|| (*str >= OS_TEXT('0') && *str <= OS_TEXT('9')) )
					{
						continue;
					}
					break;
				}
				String name = String(allocator, name_start, str - name_start);
				TokenType type = NAME;
				addToken(name, type, cur_line, name_start - line_start OS_DBG_FILEPOS);
				continue;
			}
			// parse operator
			if(0 && (*str == OS_TEXT('-') || *str == OS_TEXT('+')) && (str[1] >= OS_TEXT('0') && str[1] <= OS_TEXT('9'))){
				int i = 0;
			}else{
				int i;
				for(i = 0; i < operator_count; i++){
					size_t len = OS_STRLEN(operator_desc[i].name);
					if(OS_STRNCMP(str, operator_desc[i].name, len) == 0){
						addToken(String(allocator, str, (int)len), operator_desc[i].type, cur_line, str - line_start OS_DBG_FILEPOS);
						str += len;
						break;
					}
				}
				if(i < operator_count){
					continue;
				}
			}
			{
				OS_FLOAT fval;
				const OS_CHAR * token_start = str;
				if(parseFloat(str, fval, true)){
					TokenData * token = addToken(String(allocator, token_start, str - token_start, false, true), NUMBER, cur_line, token_start - line_start OS_DBG_FILEPOS);
					token->setFloat(fval);
					continue;
				}
			}

			error = ERROR_SYNTAX;
			cur_pos = str - line_start;
			return false;
		}
	}
	// PrintTokens();
	return true;
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::Core::Compiler::ExpressionList::ExpressionList(OS * p_allocator)
{
	allocator = p_allocator;
}

OS::Core::Compiler::ExpressionList::~ExpressionList()
{
	allocator->vectorDeleteItems(*this);
	allocator->vectorClear(*this);
}

bool OS::Core::Compiler::ExpressionList::isValue() const
{
	return count > 0 && buf[count-1]->isValue();
}

bool OS::Core::Compiler::ExpressionList::isClear() const
{
	return count <= 0 || buf[count-1]->isClear();
}

bool OS::Core::Compiler::ExpressionList::isWriteable() const
{
	return count > 0 && buf[count-1]->isWriteable();
}

OS::Core::Compiler::Expression * OS::Core::Compiler::ExpressionList::add(Expression * exp OS_DBG_FILEPOS_DECL)
{
	allocator->vectorAddItem(*this, exp OS_DBG_FILEPOS_PARAM);
	return exp;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::ExpressionList::removeIndex(int i)
{
	Expression * exp = (*this)[i];
	allocator->vectorRemoveAtIndex(*this, i);
	return exp;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::ExpressionList::removeLast()
{
	return removeIndex(count-1);
}

void OS::Core::Compiler::ExpressionList::swap(ExpressionList& list)
{
	OS_ASSERT(allocator == list.allocator);

	Expression ** save_buf = buf;
	int save_count = count;
	int save_capacity = capacity;

	buf = list.buf;
	count = list.count;
	capacity = list.capacity;

	list.buf = save_buf;
	list.count = save_count;
	list.capacity = save_capacity;
}

// =====================================================================

OS::Core::Compiler::LocalVarDesc::LocalVarDesc()
{
	up_count = 0;
	up_scope_count = 0;
	index = 0;
	type = LOCAL_GENERIC;
}

OS::Core::Compiler::Expression::Expression(ExpressionType p_type, TokenData * p_token): list(p_token->getAllocator())
{
	token = p_token->retain();
	type = p_type;
	ret_values = 0;
	active_locals = 0;
	slots.a = slots.b = slots.c = 0;
}

OS::Core::Compiler::Expression::Expression(ExpressionType p_type, TokenData * p_token, Expression * e1 OS_DBG_FILEPOS_DECL): list(p_token->getAllocator())
{
	token = p_token->retain();
	type = p_type;
	list.add(e1 OS_DBG_FILEPOS_PARAM);
	ret_values = 0;
	active_locals = 0;
	slots.a = slots.b = slots.c = 0;
}

OS::Core::Compiler::Expression::Expression(ExpressionType p_type, TokenData * p_token, Expression * e1, Expression * e2 OS_DBG_FILEPOS_DECL): list(p_token->getAllocator())
{
	token = p_token->retain();
	type = p_type;
	list.add(e1 OS_DBG_FILEPOS_PARAM);
	list.add(e2 OS_DBG_FILEPOS_PARAM);
	ret_values = 0;
	active_locals = 0;
	slots.a = slots.b = slots.c = 0;
}

OS::Core::Compiler::Expression::Expression(ExpressionType p_type, TokenData * p_token, Expression * e1, Expression * e2, Expression * e3 OS_DBG_FILEPOS_DECL): list(p_token->getAllocator())
{
	token = p_token->retain();
	type = p_type;
	list.add(e1 OS_DBG_FILEPOS_PARAM);
	list.add(e2 OS_DBG_FILEPOS_PARAM);
	list.add(e3 OS_DBG_FILEPOS_PARAM);
	ret_values = 0;
	active_locals = 0;
	slots.a = slots.b = slots.c = 0;
}


OS::Core::Compiler::Expression::~Expression()
{
	token->release();
}

bool OS::Core::Compiler::Expression::isConstValue() const
{
	switch(type){
		// case EXP_TYPE_CODE_LIST:
		// 	return list.count ? list[list.count-1]->isValue() : false;

	case EXP_TYPE_CONST_STRING:
	case EXP_TYPE_CONST_NUMBER:
	case EXP_TYPE_CONST_NULL:
	case EXP_TYPE_CONST_TRUE:
	case EXP_TYPE_CONST_FALSE:
		// case EXP_TYPE_NAME:
		// case EXP_TYPE_CALL:
		OS_ASSERT(ret_values == 1);
		return true;
	}
	return false;
}

bool OS::Core::Compiler::Expression::isValue() const
{
	return ret_values > 0;
}

bool OS::Core::Compiler::Expression::isClear() const
{
	return ret_values == 0;
}

bool OS::Core::Compiler::Expression::isWriteable() const
{
	switch(type){
		// case EXP_TYPE_CODE_LIST:
		// 	return list.count ? list[list.count-1]->isWriteable() : false;

	case EXP_TYPE_NAME:
	case EXP_TYPE_INDIRECT:
	case EXP_TYPE_CALL_DIM:
	case EXP_TYPE_CALL_METHOD:
		return true;

	case EXP_TYPE_PARAMS:
		for(int i = 0; i < list.count; i++){
			if(list[i]->type == EXP_TYPE_PARAMS || !list[i]->isWriteable()){
				return false;
			}
		}
		return true;
	}
	return false;
}

bool OS::Core::Compiler::Expression::isOperator() const
{
	return isBinaryOperator() || isUnaryOperator();
}

bool OS::Core::Compiler::Expression::isUnaryOperator() const
{
	switch(type){
	case EXP_TYPE_LOGIC_BOOL:	// !!
	case EXP_TYPE_LOGIC_NOT:	// !
	case EXP_TYPE_PLUS:			// +
	case EXP_TYPE_NEG:			// -
	case EXP_TYPE_LENGTH:		// #
		// case EXP_TYPE_INC:	// ++
		//case EXP_TYPE_DEC:	// --
	case EXP_TYPE_PRE_INC:		// ++
	case EXP_TYPE_PRE_DEC:		// --
	case EXP_TYPE_POST_INC:		// ++
	case EXP_TYPE_POST_DEC:		// --
	case EXP_TYPE_BIT_NOT:		// ~
		return true;
	}
	return false;
}

bool OS::Core::Compiler::Expression::isLogicOperator() const
{
	switch(type){
	case EXP_TYPE_LOGIC_BOOL:	// !!
	case EXP_TYPE_LOGIC_NOT:	// !

	case EXP_TYPE_LOGIC_AND: // &&
	case EXP_TYPE_LOGIC_OR:  // ||

	case EXP_TYPE_LOGIC_PTR_EQ:  // ===
	case EXP_TYPE_LOGIC_PTR_NE:  // !==
	case EXP_TYPE_LOGIC_EQ:  // ==
	case EXP_TYPE_LOGIC_NE:  // !=
	case EXP_TYPE_LOGIC_GE:  // >=
	case EXP_TYPE_LOGIC_LE:  // <=
	case EXP_TYPE_LOGIC_GREATER: // >
	case EXP_TYPE_LOGIC_LESS:    // <
		return true;
	}
	return false;
}

bool OS::Core::Compiler::Expression::isBinaryOperator() const
{
	switch(type){
	case EXP_TYPE_INDIRECT:

	case EXP_TYPE_ASSIGN:

	case EXP_TYPE_PARAMS:
	case EXP_TYPE_QUESTION:
	case EXP_TYPE_IN:
	case EXP_TYPE_ISPROTOTYPEOF:
	case EXP_TYPE_IS:
	case EXP_TYPE_CONCAT: // ..

	case EXP_TYPE_LOGIC_AND: // &&
	case EXP_TYPE_LOGIC_OR:  // ||

	case EXP_TYPE_LOGIC_PTR_EQ:  // ===
	case EXP_TYPE_LOGIC_PTR_NE:  // !==
	case EXP_TYPE_LOGIC_EQ:  // ==
	case EXP_TYPE_LOGIC_NE:  // !=
	case EXP_TYPE_LOGIC_GE:  // >=
	case EXP_TYPE_LOGIC_LE:  // <=
	case EXP_TYPE_LOGIC_GREATER: // >
	case EXP_TYPE_LOGIC_LESS:    // <

	case EXP_TYPE_BIT_AND: // &
	case EXP_TYPE_BIT_OR:  // |
	case EXP_TYPE_BIT_XOR: // ^

	case EXP_TYPE_BIT_AND_ASSIGN: // &=
	case EXP_TYPE_BIT_OR_ASSIGN:  // |=
	case EXP_TYPE_BIT_XOR_ASSIGN: // ^=
	case EXP_TYPE_BIT_NOT_ASSIGN: // ~=

	case EXP_TYPE_ADD: // +
	case EXP_TYPE_SUB: // -
	case EXP_TYPE_MUL: // *
	case EXP_TYPE_DIV: // /
	case EXP_TYPE_MOD: // %
	case EXP_TYPE_LSHIFT: // <<
	case EXP_TYPE_RSHIFT: // >>
	case EXP_TYPE_POW: // **

	case EXP_TYPE_ADD_ASSIGN: // +=
	case EXP_TYPE_SUB_ASSIGN: // -=
	case EXP_TYPE_MUL_ASSIGN: // *=
	case EXP_TYPE_DIV_ASSIGN: // /=
	case EXP_TYPE_MOD_ASSIGN: // %=
	case EXP_TYPE_LSHIFT_ASSIGN: // <<=
	case EXP_TYPE_RSHIFT_ASSIGN: // >>=
	case EXP_TYPE_POW_ASSIGN: // **=
		return true;
	}
	return isAssignOperator();
}

bool OS::Core::Compiler::Expression::isAssignOperator() const
{
	switch(type){
	case EXP_TYPE_ASSIGN: // =

	case EXP_TYPE_BIT_AND_ASSIGN: // &=
	case EXP_TYPE_BIT_OR_ASSIGN:  // |=
	case EXP_TYPE_BIT_XOR_ASSIGN: // ^=
	case EXP_TYPE_BIT_NOT_ASSIGN: // ~=

	case EXP_TYPE_ADD_ASSIGN: // +=
	case EXP_TYPE_SUB_ASSIGN: // -=
	case EXP_TYPE_MUL_ASSIGN: // *=
	case EXP_TYPE_DIV_ASSIGN: // /=
	case EXP_TYPE_MOD_ASSIGN: // %=
	case EXP_TYPE_LSHIFT_ASSIGN: // <<=
	case EXP_TYPE_RSHIFT_ASSIGN: // >>=
	case EXP_TYPE_POW_ASSIGN: // **=
		return true;
	}
	return false;
}

OS::Core::String OS::Core::Compiler::Expression::getSlotStr(OS::Core::Compiler * compiler, Scope * scope, int slot_num, int up_count)
{
	// OS_ASSERT(slot_num);
	OS * allocator = compiler->allocator;
	for(; up_count > 0; up_count--){
		scope = scope->function->parent;
	}
	if(slot_num < 0){
		slot_num = -slot_num-1;
		if(slot_num == CONST_NULL){
			return String(allocator, OS_TEXT("const null"));
		}
		if(slot_num == CONST_FALSE){
			return String(allocator, OS_TEXT("const false"));
		}
		if(slot_num == CONST_TRUE){
			return String(allocator, OS_TEXT("const true"));
		}
		slot_num -= CONST_STD_VALUES;
		if(slot_num < compiler->prog_numbers.count){
			return String::format(allocator, OS_TEXT("const number %g"), compiler->prog_numbers[slot_num]);
		}
		slot_num -= compiler->prog_numbers.count;
		return String::format(allocator, OS_TEXT("const string \"%s\""), compiler->prog_strings[slot_num]);
	}
	if(slot_num >= scope->function->num_locals){
		return allocator->core->strings->var_temp_prefix;
	}
	for(;;){
		for(int i = scope->locals.count-1; i >= 0; i--){
			const Scope::LocalVar& local_var = scope->locals[i];
			if(local_var.index == slot_num){
				if(local_var.name == allocator->core->strings->var_temp_prefix){
					OS_ASSERT(false);
					return allocator->core->strings->var_temp_prefix;
				}
				return String::format(allocator, slot_num < scope->function->num_params ? OS_TEXT("param %s") : OS_TEXT("var %s"), local_var.name.toChar());
			}
		}
		if(scope == scope->function){
			OS_ASSERT(false);
			break;
		}
		scope = scope->parent;
	}
	/*
	const Scope::LocalVar& local_var = scope->function->func_locals[slot_num];
	if(local_var.name == allocator->core->strings->var_temp_prefix){
		return allocator->core->strings->var_temp_prefix;
	}
	return String::format(allocator, slot_num < scope->function->num_params ? OS_TEXT("param %s") : OS_TEXT("var %s"), local_var.name.toChar());
	*/
	return allocator->core->strings->var_temp_prefix; // shut up compiler
}

void OS::Core::Compiler::Expression::debugPrint(StringBuffer& out, OS::Core::Compiler * compiler, Scope * scope, int depth)
{
	OS * allocator = getAllocator();
	compiler->debugPrintSourceLine(out, token);

	int i;
	OS_CHAR * spaces = (OS_CHAR*)alloca(sizeof(OS_CHAR)*(depth*2+1));
	for(i = 0; i < depth*2; i++){
		spaces[i] = OS_TEXT(' ');
	}
	spaces[i] = OS_TEXT('\0');

	const OS_CHAR * type_name;
	switch(type){
	default:
		OS_ASSERT(false);
		break;

	case EXP_TYPE_NOP:
		for(i = 0; i < list.count; i++){
			list[i]->debugPrint(out, compiler, scope, depth);
		}
		// out += String::format(allocator, OS_TEXT("%snop\n"), spaces);
		break;

	case EXP_TYPE_CODE_LIST:
		type_name = OS::Core::Compiler::getExpName(type);
		// out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, type_name);
		for(i = 0; i < list.count; i++){
			if(i > 0){
				// out += OS_TEXT("\n");
			}
			// OS_ASSERT(i+1 == list.count ? list[i]->ret_values == ret_values : list[i]->ret_values == 0);
			list[i]->debugPrint(out, compiler, scope, depth);
		}
		// out += String::format(allocator, OS_TEXT("%send %s ret values %d\n"), spaces, type_name, ret_values);
		break;

	case EXP_TYPE_IF:
		OS_ASSERT(list.count == 2 || list.count == 3);
		out += String::format(allocator, OS_TEXT("%sbegin if\n"), spaces);
		out += String::format(allocator, OS_TEXT("%s  begin bool exp\n"), spaces);
		list[0]->debugPrint(out, compiler, scope, depth+2);
		out += String::format(allocator, OS_TEXT("%s  end bool exp\n"), spaces);
		out += String::format(allocator, OS_TEXT("%s  begin then\n"), spaces);
		list[1]->debugPrint(out, compiler, scope, depth+2);
		out += String::format(allocator, OS_TEXT("%s  end then\n"), spaces);
		if(list.count == 3){
			out += String::format(allocator, OS_TEXT("%s  begin else\n"), spaces);
			list[2]->debugPrint(out, compiler, scope, depth+2);
			out += String::format(allocator, OS_TEXT("%s  end else\n"), spaces);
		}
		out += String::format(allocator, OS_TEXT("%send if ret values %d\n"), spaces, ret_values);
		break;

	case EXP_TYPE_QUESTION:
		OS_ASSERT(list.count == 3);
		out += String::format(allocator, OS_TEXT("%sbegin question\n"), spaces);
		out += String::format(allocator, OS_TEXT("%s  begin bool exp\n"), spaces);
		list[0]->debugPrint(out, compiler, scope, depth+2);
		out += String::format(allocator, OS_TEXT("%s  end bool exp\n"), spaces);
		out += String::format(allocator, OS_TEXT("%s  begin then value\n"), spaces);
		list[1]->debugPrint(out, compiler, scope, depth+2);
		out += String::format(allocator, OS_TEXT("%s  end then value\n"), spaces);
		out += String::format(allocator, OS_TEXT("%s  begin else value\n"), spaces);
		list[2]->debugPrint(out, compiler, scope, depth+2);
		out += String::format(allocator, OS_TEXT("%s  end else value\n"), spaces);
		out += String::format(allocator, OS_TEXT("%send question ret values %d\n"), spaces, ret_values);
		break;

	/*
	case EXP_TYPE_CONST_NUMBER:
	case EXP_TYPE_CONST_STRING:
		{
			const OS_CHAR * end = OS_TEXT("");
			switch(token->type){
			case Tokenizer::NUMBER: type_name = OS_TEXT("number "); break;
			case Tokenizer::STRING: type_name = OS_TEXT("string \""); end = OS_TEXT("\""); break;
			case Tokenizer::NAME: type_name = OS_TEXT("string \""); end = OS_TEXT("\""); break;
			default: type_name = OS_TEXT("???"); break;
			}
			out += String::format(allocator, OS_TEXT("%spush const %s%s%s\n"), spaces, type_name, token->str.toChar(), end);
		}
		break;

	case EXP_TYPE_CONST_NULL:
	case EXP_TYPE_CONST_TRUE:
	case EXP_TYPE_CONST_FALSE:
		out += String::format(allocator, OS_TEXT("%s%s\n"), spaces, getExpName(type));
		break;

	case EXP_TYPE_NAME:
		out += String::format(allocator, OS_TEXT("%sname %s\n"), spaces, token->str.toChar());
		break;
	*/

	case EXP_TYPE_PARAMS:
		// out += String::format(allocator, OS_TEXT("%sbegin params %d\n"), spaces, list.count);
		for(i = 0; i < list.count; i++){
			if(i > 0){
				// out += String::format(allocator, OS_TEXT("%s  ,\n"), spaces);
			}
			list[i]->debugPrint(out, compiler, scope, depth);
		}
		// out += String::format(allocator, OS_TEXT("%send params ret values %d\n"), spaces, ret_values);
		break;

	case EXP_TYPE_ARRAY:
		// out += String::format(allocator, OS_TEXT("%sbegin array %d\n"), spaces, list.count);
		out += String::format(allocator, OS_TEXT("%snew array %d: %s (%d)\n"), spaces, list.count,
			getSlotStr(compiler, scope, slots.a).toChar(), slots.a);
		for(i = 0; i < list.count; i++){
			if(i > 0){
				// out += String::format(allocator, OS_TEXT("%s  ,\n"), spaces);
			}
			list[i]->debugPrint(out, compiler, scope, depth);
		}
		// out += String::format(allocator, OS_TEXT("%send array\n"), spaces);
		break;

	case EXP_TYPE_OBJECT:
		// out += String::format(allocator, OS_TEXT("%sbegin object %d\n"), spaces, list.count);
		out += String::format(allocator, OS_TEXT("%snew object %d: %s (%d)\n"), spaces, list.count,
			getSlotStr(compiler, scope, slots.a).toChar(), slots.a);
		for(i = 0; i < list.count; i++){
			if(i > 0){
				// out += String::format(allocator, OS_TEXT("%s  ,\n"), spaces);
			}
			list[i]->debugPrint(out, compiler, scope, depth);
		}
		// out += String::format(allocator, OS_TEXT("%send object\n"), spaces);
		break;

	/*
	case EXP_TYPE_OBJECT_SET_BY_NAME:
		OS_ASSERT(list.count == 1);
		out += String::format(allocator, OS_TEXT("%sbegin set by name\n"), spaces);
		list[0]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send set by name: [%s]\n"), spaces, token->str.toChar());
		break;

	case EXP_TYPE_OBJECT_SET_BY_INDEX:
		OS_ASSERT(list.count == 1);
		out += String::format(allocator, OS_TEXT("%sbegin set by index\n"), spaces);
		list[0]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send set by index: [%d]\n"), spaces, (int)token->getFloat());
		break;

	case EXP_TYPE_OBJECT_SET_BY_EXP:
		OS_ASSERT(list.count == 2);
		out += String::format(allocator, OS_TEXT("%sbegin set by exp\n"), spaces);
		list[0]->debugPrint(out, compiler, scope, depth+1);
		list[1]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send set by exp\n"), spaces);
		break;

	case EXP_TYPE_OBJECT_SET_BY_AUTO_INDEX:
		OS_ASSERT(list.count == 1);
		out += String::format(allocator, OS_TEXT("%sbegin set auto index\n"), spaces);
		list[0]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send set auto index\n"), spaces);
		break;
	*/

	case EXP_TYPE_FUNCTION:
		{
			// OS_ASSERT(list.count >= 1);
			Scope * scope = dynamic_cast<Scope*>(this);
			OS_ASSERT(scope);
			out += String::format(allocator, OS_TEXT("%sbegin function\n"), spaces);
			if(scope->num_locals > 0){
				out += String::format(allocator, OS_TEXT("%s  begin locals %d, stack %d\n"), spaces, scope->num_locals, scope->stack_size - scope->num_locals);
				for(i = 0; i < scope->locals.count; i++){
					if(scope->locals[i].name == allocator->core->strings->var_temp_prefix){
						continue;
					}
					out += String::format(allocator, OS_TEXT("%s    %d %s%s\n"), spaces, 
						scope->locals[i].index,
						scope->locals[i].name.toChar(),
						i < scope->num_params ? OS_TEXT(" (param)") : OS_TEXT("")
						);
				}
				out += String::format(allocator, OS_TEXT("%s  end locals\n"), spaces);
			}
			for(i = 0; i < list.count; i++){
				if(i > 0){
					out += OS_TEXT("\n");
				}
				list[i]->debugPrint(out, compiler, scope, depth+1);
			}
			out += String::format(allocator, OS_TEXT("%send function: %s (%d), index %d\n"), spaces, 
				scope->function->parent ? scope->function->parent->getSlotStr(compiler, scope->function->parent, slots.a).toChar() : OS_TEXT("<<->>"), 
				slots.a, slots.b);
			break;
		}

	case EXP_TYPE_SCOPE:
	case EXP_TYPE_LOOP_SCOPE:
		{
			// OS_ASSERT(list.count >= 1);
			Scope * scope = dynamic_cast<Scope*>(this);
			OS_ASSERT(scope);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, exp_name);
			if(scope->locals.count > 0){
				out += String::format(allocator, OS_TEXT("%s  begin locals %d\n"), spaces, scope->locals.count);
				for(i = 0; i < scope->locals.count; i++){
					if(scope->locals[i].name == allocator->core->strings->var_temp_prefix){
						continue;
					}
					out += String::format(allocator, OS_TEXT("%s    %d %s%s\n"), spaces, 
						scope->locals[i].index,
						scope->locals[i].name.toChar(),
						i < scope->num_params ? OS_TEXT(" (param)") : OS_TEXT("")
						);
				}
				out += String::format(allocator, OS_TEXT("%s  end locals\n"), spaces);
			}
			for(i = 0; i < list.count; i++){
				if(i > 0){
					out += OS_TEXT("\n");
				}
				list[i]->debugPrint(out, compiler, scope, depth+1);
			}
			out += String::format(allocator, OS_TEXT("%send %s ret values %d\n"), spaces, exp_name, ret_values);
			break;
		}

	case EXP_TYPE_RETURN:
		if(list.count > 0){
			// out += String::format(allocator, OS_TEXT("%sbegin return\n"), spaces);
			for(i = 0; i < list.count; i++){
				if(i > 0){
					// out += String::format(allocator, OS_TEXT("%s  ,\n"), spaces);
				}
				list[i]->debugPrint(out, compiler, scope, depth+1);
			}
			// out += String::format(allocator, OS_TEXT("%send return values %d\n"), spaces, ret_values);
		}
		OS_ASSERT((slots.a > 0 && slots.b > 0) || (!slots.a && !slots.b));
		out += String::format(allocator, OS_TEXT("%sreturn: %s (%d), count %d\n"), spaces, 
			slots.a ? getSlotStr(compiler, scope, slots.a).toChar() : OS_TEXT("<<->>"), 
			slots.a, slots.b);
		break;

	case EXP_TYPE_GET_UPVALUE_VAR:
		out += String::format(allocator, OS_TEXT("%sget upvalue: %s (%d) = %s (%d %d)\n"), spaces,
			getSlotStr(compiler, scope, slots.a).toChar(), slots.a, 
			getSlotStr(compiler, scope, slots.b, slots.c).toChar(), slots.b, slots.c);
		break;

	case EXP_TYPE_SET_UPVALUE_VAR:
		OS_ASSERT(list.count == 1);
		list[0]->debugPrint(out, compiler, scope, depth);
		out += String::format(allocator, OS_TEXT("%sset upvalue: %s (%d %d) = %s (%d)\n"), spaces,
			getSlotStr(compiler, scope, slots.a, slots.c).toChar(), slots.a, slots.c, 
			getSlotStr(compiler, scope, slots.b).toChar(), slots.b);
		break;
					
	case EXP_TYPE_MOVE:
	case EXP_TYPE_GET_XCONST:
		{
			for(i = 0; i < list.count; i++){
				if(i > 0){
					// out += String::format(allocator, OS_TEXT("%s  ,\n"), spaces);
				}
				list[i]->debugPrint(out, compiler, scope, depth);
			}
			if(slots.a != slots.b && slots.a){
				OS_ASSERT(slots.a > 0);
				const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
				out += String::format(allocator, OS_TEXT("%s%s: %s (%d) = %s (%d)\n"), spaces, exp_name,
					getSlotStr(compiler, scope, slots.a).toChar(), slots.a, 
					getSlotStr(compiler, scope, slots.b).toChar(), slots.b);
			}
			break;
		}

	case EXP_TYPE_BREAK:
		OS_ASSERT(list.count == 0);
		out += String::format(allocator, OS_TEXT("%sbreak\n"), spaces);
		break;

	case EXP_TYPE_CONTINUE:
		OS_ASSERT(list.count == 0);
		out += String::format(allocator, OS_TEXT("%scontinue\n"), spaces);
		break;

	case EXP_TYPE_DEBUGGER:
		OS_ASSERT(list.count == 0);
		out += String::format(allocator, OS_TEXT("%sdebugger\n"), spaces);
		break;

	/*
	case EXP_TYPE_DEBUG_LOCALS:
		out += String::format(allocator, OS_TEXT("%sbegin debug locals\n"), spaces);
		for(i = 0; i < list.count; i++){
			if(i > 0){
				out += String::format(allocator, OS_TEXT("%s  ,\n"), spaces);
			}
			list[i]->debugPrint(out, compiler, scope, depth+1);
		}
		out += String::format(allocator, OS_TEXT("%send debug locals\n"), spaces);
		break;
	*/

	case EXP_TYPE_TAIL_CALL:
		OS_ASSERT(list.count == 2);
		out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		list[0]->debugPrint(out, compiler, scope, depth+1);
		list[1]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		break;

	case EXP_TYPE_TAIL_CALL_METHOD:
		OS_ASSERT(list.count == 2);
		out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		list[0]->debugPrint(out, compiler, scope, depth+1);
		list[1]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		break;

	case EXP_TYPE_CALL_DIM:
		// case EXP_TYPE_GET_DIM:
	// case EXP_TYPE_CALL_METHOD:
	case EXP_TYPE_GET_THIS_PROPERTY_BY_STRING:
	case EXP_TYPE_GET_PROPERTY_BY_LOCALS:
	case EXP_TYPE_GET_PROPERTY_BY_LOCAL_AND_NUMBER:
	case EXP_TYPE_GET_PROPERTY_AUTO_CREATE:
		// case EXP_TYPE_GET_PROPERTY_DIM:
		// case EXP_TYPE_SET_ENV_VAR_DIM:
	// case EXP_TYPE_EXTENDS:
		OS_ASSERT(list.count == 2);
		out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		list[0]->debugPrint(out, compiler, scope, depth+1);
		list[1]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send %s ret values %d\n"), spaces, OS::Core::Compiler::getExpName(type), ret_values);
		break;

	case EXP_TYPE_GET_PROPERTY:
		OS_ASSERT(list.count == 0 || list.count == 2);
		// out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		if(list.count == 2){
			list[0]->debugPrint(out, compiler, scope, depth);
			list[1]->debugPrint(out, compiler, scope, depth);
		}
		out += String::format(allocator, OS_TEXT("%s%s: %s (%d) = %s (%d) [%s (%d)]\n"), spaces, OS::Core::Compiler::getExpName(type), 
					getSlotStr(compiler, scope, slots.a).toChar(), slots.a, 
					getSlotStr(compiler, scope, slots.b).toChar(), slots.b, 
					getSlotStr(compiler, scope, slots.c).toChar(), slots.c);
		break;

	/*
	case EXP_TYPE_DELETE:
		OS_ASSERT(list.count == 2);
		out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		list[0]->debugPrint(out, compiler, scope, depth+1);
		list[1]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		break;

	case EXP_TYPE_CLONE:
		OS_ASSERT(list.count == 1);
		out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		list[0]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		break;
	*/

	case EXP_TYPE_VALUE:
		OS_ASSERT(list.count == 1);
		out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		list[0]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		break;

	case EXP_TYPE_POP_VALUE:
		OS_ASSERT(list.count == 1);
		out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		list[0]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send %s ret values %d\n"), spaces, OS::Core::Compiler::getExpName(type), ret_values);
		break;

	case EXP_TYPE_SUPER:
		OS_ASSERT(list.count == 0);
		out += String::format(allocator, OS_TEXT("%ssuper\n"), spaces);
		break;

	/*
	case EXP_TYPE_TYPE_OF:
	case EXP_TYPE_VALUE_OF:
	case EXP_TYPE_NUMBER_OF:
	case EXP_TYPE_STRING_OF:
	case EXP_TYPE_ARRAY_OF:
	case EXP_TYPE_OBJECT_OF:
	case EXP_TYPE_USERDATA_OF:
	case EXP_TYPE_FUNCTION_OF:
	*/
	case EXP_TYPE_PLUS:			// +
	case EXP_TYPE_NEG:			// -
	// case EXP_TYPE_LENGTH:		// #
	case EXP_TYPE_LOGIC_BOOL:	// !!
	case EXP_TYPE_LOGIC_NOT:	// !
	case EXP_TYPE_BIT_NOT:		// ~
	/*
	case EXP_TYPE_PRE_INC:		// ++
	case EXP_TYPE_PRE_DEC:		// --
	case EXP_TYPE_POST_INC:		// ++
	case EXP_TYPE_POST_DEC:		// --
	*/
		{
			OS_ASSERT(list.count == 1);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			// out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, exp_name);
			list[0]->debugPrint(out, compiler, scope, depth);
			// out += String::format(allocator, OS_TEXT("%send %s\n"), spaces, exp_name);
			out += String::format(allocator, OS_TEXT("%s%s (%d) = [%s] %s (%d)\n"), spaces,
				getSlotStr(compiler, scope, slots.a).toChar(), slots.a, exp_name,
				getSlotStr(compiler, scope, slots.b).toChar(), slots.b);
			break;
		}

	case EXP_TYPE_INDIRECT:
	case EXP_TYPE_ASSIGN:
	case EXP_TYPE_CONCAT: // ..
	case EXP_TYPE_IN:
	case EXP_TYPE_ISPROTOTYPEOF:
	case EXP_TYPE_IS:
	case EXP_TYPE_BIT_AND: // &
	case EXP_TYPE_BIT_OR:  // |
	case EXP_TYPE_BIT_XOR: // ^
		// case EXP_TYPE_BIT_NOT: // ~
	case EXP_TYPE_BIT_AND_ASSIGN: // &=
	case EXP_TYPE_BIT_OR_ASSIGN:  // |=
	case EXP_TYPE_BIT_XOR_ASSIGN: // ^=
	case EXP_TYPE_BIT_NOT_ASSIGN: // ~=
	case EXP_TYPE_ADD: // +
	case EXP_TYPE_SUB: // -
	case EXP_TYPE_MUL: // *
	case EXP_TYPE_DIV: // /
	case EXP_TYPE_MOD: // %
	case EXP_TYPE_LSHIFT: // <<
	case EXP_TYPE_RSHIFT: // >>
	case EXP_TYPE_POW: // **

	case EXP_TYPE_ADD_ASSIGN: // +=
	case EXP_TYPE_SUB_ASSIGN: // -=
	case EXP_TYPE_MUL_ASSIGN: // *=
	case EXP_TYPE_DIV_ASSIGN: // /=
	case EXP_TYPE_MOD_ASSIGN: // %=
	case EXP_TYPE_LSHIFT_ASSIGN: // <<=
	case EXP_TYPE_RSHIFT_ASSIGN: // >>=
	case EXP_TYPE_POW_ASSIGN: // **=
		{
			OS_ASSERT(list.count == 2);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			// out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, exp_name);
			list[0]->debugPrint(out, compiler, scope, depth);
			list[1]->debugPrint(out, compiler, scope, depth);
			/* out += String::format(allocator, OS_TEXT("%send %s: %d (%s), %d (%s) => %d (%s)\n"), spaces, exp_name, 
				slots.a, getSlotStr(compiler, scope, slots.a).toChar(), 
				slots.b, getSlotStr(compiler, scope, slots.b).toChar(), 
				slots.c, getSlotStr(compiler, scope, slots.c).toChar()); */
			out += String::format(allocator, OS_TEXT("%s%s (%d) = %s (%d) [%s] %s (%d)\n"), spaces,  
				getSlotStr(compiler, scope, slots.a).toChar(), slots.a, 
				getSlotStr(compiler, scope, slots.b).toChar(), slots.b, exp_name,
				getSlotStr(compiler, scope, slots.c).toChar(), slots.c);
			break;
		}

	case EXP_TYPE_LOGIC_PTR_EQ:  // ===
	case EXP_TYPE_LOGIC_PTR_NE:  // !==
	case EXP_TYPE_LOGIC_EQ:  // ==
	case EXP_TYPE_LOGIC_NE:  // !=
	case EXP_TYPE_LOGIC_GE:  // >=
	case EXP_TYPE_LOGIC_LE:  // <=
	case EXP_TYPE_LOGIC_GREATER: // >
	case EXP_TYPE_LOGIC_LESS:    // <
		{
			OS_ASSERT(list.count == 2);
			list[0]->debugPrint(out, compiler, scope, depth);
			list[1]->debugPrint(out, compiler, scope, depth);
			out += String::format(allocator, OS_TEXT("%s%s (%d) = %s (%d) [%s] %s (%d)\n"), spaces,  
				getSlotStr(compiler, scope, slots.a).toChar(), slots.a, 
				getSlotStr(compiler, scope, slots.a).toChar(), slots.a, OS::Core::Compiler::getExpName(type),
				getSlotStr(compiler, scope, slots.a+1).toChar(), slots.a+1);
			break;
		}

	case EXP_TYPE_LOGIC_AND: // &&
	case EXP_TYPE_LOGIC_OR:  // ||
		{
			OS_ASSERT(list.count == 2);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			// out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, exp_name);
			list[0]->debugPrint(out, compiler, scope, depth);
			list[1]->debugPrint(out, compiler, scope, depth);
			/* out += String::format(allocator, OS_TEXT("%send %s: %d (%s), %d (%s) => %d (%s)\n"), spaces, exp_name, 
				slots.a, getSlotStr(compiler, scope, slots.a).toChar(), 
				slots.b, getSlotStr(compiler, scope, slots.b).toChar(), 
				slots.c, getSlotStr(compiler, scope, slots.c).toChar()); */
			out += String::format(allocator, OS_TEXT("%s%s: %s (%d)\n"), spaces, exp_name,
				getSlotStr(compiler, scope, slots.a).toChar(), slots.a);
			break;
		}

	case EXP_TYPE_CALL_METHOD:
	case EXP_TYPE_SUPER_CALL:
	case EXP_TYPE_CALL:
	case EXP_TYPE_CALL_AUTO_PARAM:
		{
			OS_ASSERT(list.count == 2);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, exp_name);
			list[0]->debugPrint(out, compiler, scope, depth+1);
			list[1]->debugPrint(out, compiler, scope, depth+1);
			out += String::format(allocator, OS_TEXT("%send %s: start %d, params %d, ret values %d\n"), spaces, exp_name, slots.a, slots.b, slots.c);
			break;
		}

	/* case EXP_TYPE_CALL:
	case EXP_TYPE_CALL_AUTO_PARAM:
		OS_ASSERT(list.count == 2);
		out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, OS::Core::Compiler::getExpName(type));
		list[0]->debugPrint(out, compiler, scope, depth+1);
		list[1]->debugPrint(out, compiler, scope, depth+1);
		out += String::format(allocator, OS_TEXT("%send %s ret values %d\n"), spaces, OS::Core::Compiler::getExpName(type), ret_values);
		break; */

	/*
	case EXP_TYPE_BIN_OPERATOR_BY_LOCALS:
	case EXP_TYPE_BIN_OPERATOR_BY_LOCAL_AND_NUMBER:
		{
			OS_ASSERT(list.count == 1);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, exp_name);
			list[0]->debugPrint(out, compiler, scope, depth+1);
			out += String::format(allocator, OS_TEXT("%send %s\n"), spaces, exp_name);
			break;
		}
	*/

	case EXP_TYPE_NEW_LOCAL_VAR:
		{
			/*
			OS_ASSERT(list.count == 0);
			String info = String::format(allocator, OS_TEXT("(%d %d%s)"),
				local_var.index, local_var.up_count, 
				local_var.type == LOCAL_PARAM ? OS_TEXT(" param") : (local_var.type == LOCAL_TEMP ? OS_TEXT(" temp") : OS_TEXT("")));
			out += String::format(allocator, OS_TEXT("%snew local var %s %s\n"), spaces, token->str.toChar(), info.toChar());
			*/
			break;
		}

	case EXP_TYPE_GET_THIS:
	case EXP_TYPE_GET_ARGUMENTS:
	case EXP_TYPE_GET_REST_ARGUMENTS:
		{
			OS_ASSERT(list.count == 0);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			out += String::format(allocator, OS_TEXT("%s%s: %s (%d)\n"), spaces, exp_name, getSlotStr(compiler, scope, slots.a).toChar(), slots.a);
			break;
		}

	case EXP_TYPE_GET_LOCAL_VAR:
	case EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE:
		{
			OS_ASSERT(list.count == 0);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			String info = String::format(allocator, OS_TEXT("(%d %d%s)"),
				local_var.index, local_var.up_count, 
				local_var.type == LOCAL_PARAM ? OS_TEXT(" param") : (local_var.type == LOCAL_TEMP ? OS_TEXT(" temp") : OS_TEXT("")));
			out += String::format(allocator, OS_TEXT("%s%s %s %s\n"), spaces, exp_name, token->str.toChar(), info.toChar());
			break;
		}

	case EXP_TYPE_GET_ENV_VAR:
	case EXP_TYPE_GET_ENV_VAR_AUTO_CREATE:
		{
			OS_ASSERT(list.count == 0);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			out += String::format(allocator, OS_TEXT("%s%s %s\n"), spaces, exp_name, token->str.toChar());
			break;
		}
	/*
	case EXP_TYPE_SET_LOCAL_VAR:
	case EXP_TYPE_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCALS:
	case EXP_TYPE_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCAL_AND_NUMBER:
		{
			OS_ASSERT(list.count == 1);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			String info = String::format(allocator, OS_TEXT("(%d %d%s)"),
				local_var.index, local_var.up_count, 
				local_var.type == LOCAL_PARAM ? OS_TEXT(" param") : (local_var.type == LOCAL_TEMP ? OS_TEXT(" temp") : OS_TEXT("")));
			out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, exp_name);
			list[0]->debugPrint(out, compiler, scope, depth+1);
			out += String::format(allocator, OS_TEXT("%send %s %s %s\n"), spaces, exp_name, token->str.toChar(), info.toChar());
			break;
		}
	*/
	/*
	case EXP_TYPE_SET_ENV_VAR:
		{
			OS_ASSERT(list.count == 1);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, exp_name);
			list[0]->debugPrint(out, compiler, scope, depth+1);
			out += String::format(allocator, OS_TEXT("%send %s %s\n"), spaces, exp_name, token->str.toChar());
			break;
		}
	*/

	case EXP_TYPE_SET_PROPERTY:
		{
			OS_ASSERT(list.count >= 1 && list.count <= 3);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			for(i = 0; i < list.count; i++){
				list[i]->debugPrint(out, compiler, scope, depth);
			}
			out += String::format(allocator, OS_TEXT("%s%s: %s (%d) [%s (%d)] = %s (%d)\n"), spaces, exp_name, 
				getSlotStr(compiler, scope, slots.a).toChar(), slots.a,
				getSlotStr(compiler, scope, slots.b).toChar(), slots.b,
				getSlotStr(compiler, scope, slots.c).toChar(), slots.c);
			break;
		}

	/*
	case EXP_TYPE_SET_PROPERTY_BY_LOCALS_AUTO_CREATE:
	case EXP_TYPE_GET_SET_PROPERTY_BY_LOCALS_AUTO_CREATE:
		// case EXP_TYPE_SET_PROPERTY_DIM:
	case EXP_TYPE_SET_DIM:
		{
			OS_ASSERT(list.count == 3);
			const OS_CHAR * exp_name = OS::Core::Compiler::getExpName(type);
			out += String::format(allocator, OS_TEXT("%sbegin %s\n"), spaces, exp_name);
			list[0]->debugPrint(out, compiler, scope, depth+1);
			list[1]->debugPrint(out, compiler, scope, depth+1);
			list[2]->debugPrint(out, compiler, scope, depth+1);
			out += String::format(allocator, OS_TEXT("%send %s ret values %d\n"), spaces, exp_name, ret_values);
			break;
		}
	*/
	}
}

// =====================================================================

int OS::Core::Compiler::cacheString(Table * strings_table, Vector<String>& strings, const String& str)
{
	PropertyIndex index(str, PropertyIndex::KeepStringIndex());
	Property * prop = strings_table->get(index);
	if(prop){
		OS_ASSERT(prop->value.type == OS_VALUE_TYPE_NUMBER);
		return (int)prop->value.v.number;
	}
	prop = new (malloc(sizeof(Property) OS_DBG_FILEPOS)) Property(index);
	prop->value = Value(strings_table->count);
	allocator->core->addTableProperty(strings_table, prop);
	allocator->vectorAddItem(strings, str OS_DBG_FILEPOS);
	OS_ASSERT(strings_table->count == strings.count);
	return strings_table->count-1;
}

int OS::Core::Compiler::cacheString(const String& str)
{
	return cacheString(prog_strings_table, prog_strings, str);
}

int OS::Core::Compiler::cacheDebugString(const String& str)
{
	return cacheString(prog_debug_strings_table, prog_debug_strings, str);
}

int OS::Core::Compiler::cacheNumber(OS_NUMBER num)
{
	PropertyIndex index(num);
	Property * prop = prog_numbers_table->get(index);
	if(prop){
		OS_ASSERT(prop->value.type == OS_VALUE_TYPE_NUMBER);
		return (int)prop->value.v.number;
	}
	prop = new (malloc(sizeof(Property) OS_DBG_FILEPOS)) Property(index);
	prop->value = Value(prog_numbers_table->count);
	allocator->core->addTableProperty(prog_numbers_table, prop);
	allocator->vectorAddItem(prog_numbers, num OS_DBG_FILEPOS);
	OS_ASSERT(prog_numbers_table->count == prog_numbers.count);
	return prog_numbers_table->count-1;
}

void OS::Core::Compiler::writeDebugInfo(Expression * exp)
{
	if(prog_debug_info){
		prog_num_debug_infos++;
		prog_debug_info->writeUVariable(prog_opcodes_old->getPos());
		prog_debug_info->writeUVariable(exp->token->line+1);
		prog_debug_info->writeUVariable(exp->token->pos+1);
		prog_debug_info->writeUVariable(cacheDebugString(exp->token->str));
	}
}

bool OS::Core::Compiler::writeOpcodes(Scope * scope, ExpressionList& list)
{
	for(int i = 0; i < list.count; i++){
		if(!writeOpcodes(scope, list[i])){
			return false;
		}
	}
	return true;
}

int OS::Core::Compiler::getOpcodePos()
{
	return prog_opcodes.count;
}

int OS::Core::Compiler::writeOpcode(OS_U32 opcode)
{
	int i = prog_opcodes.count;
	allocator->vectorAddItem(prog_opcodes, opcode OS_DBG_FILEPOS);
	return i;
}

int OS::Core::Compiler::writeOpcode(int opcode, int a, int b, int c)
{
	return OS_OPCODE_ABC(opcode, a, b, c);
}

void OS::Core::Compiler::writeOpcodeAt(OS_U32 opcode, int pos)
{
	prog_opcodes[pos] = opcode;
}

bool OS::Core::Compiler::writeOpcodes(Scope * scope, Expression * exp)
{
#if 0
	int i;
	switch(exp->type){
	default:
		{
			ExpressionType exp_type = exp->type;
			OS_ASSERT(false);
			break;
		}

	case EXP_TYPE_NOP:
	case EXP_TYPE_NEW_LOCAL_VAR:
		break;

	case EXP_TYPE_VALUE:
	case EXP_TYPE_CODE_LIST:
		if(!writeOpcodes(scope, exp->list)){
			return false;
		}
		break;

	case EXP_TYPE_FUNCTION:
		{
			Scope * scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(scope);
			writeDebugInfo(exp);
			
			int prog_func_index = scope->prog_func_index; // prog_functions.indexOf(scope);
			OS_ASSERT(prog_func_index >= 0);
			
			int pos = writeOpcode(Program::OP_NEW_FUNCTION, prog_func_index, 0, 0);

			allocator->vectorReserveCapacity(scope->locals_compiled, scope->num_locals OS_DBG_FILEPOS);
			scope->locals_compiled.count = scope->num_locals;

			scope->opcodes_pos = getOpcodePos();
			if(!writeOpcodes(scope, exp->list)){
				return false;
			}
			writeOpcode(Program::OP_RETURN, 0, 0, 1); // return auto
			scope->opcodes_size = getOpcodePos() - scope->opcodes_pos;

			for(i = 0; i < scope->locals.count; i++){
				Scope::LocalVar& var = scope->locals[i];
				Scope::LocalVarCompiled& var_scope = scope->locals_compiled[var.index];
				var_scope.cached_name_index = cacheString(var.name);
				var_scope.start_code_pos = scope->opcodes_pos;
				var_scope.end_code_pos = getOpcodePos();
			}
			break;
		}

	case EXP_TYPE_SCOPE:
	case EXP_TYPE_LOOP_SCOPE:
		{
			Scope * scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(scope);
			int start_code_pos = getOpcodePos();
			if(!writeOpcodes(scope, exp->list)){
				return false;
			}
			if(exp->type == EXP_TYPE_LOOP_SCOPE){
				writeJumpOpcode(start_code_pos - getOpcodePos() - 1);
				scope->fixLoopBreaks(this, start_code_pos, getOpcodePos());
			}else{
				OS_ASSERT(scope->loop_breaks.count == 0);
			}
			for(i = 0; i < scope->locals.count; i++){
				Scope::LocalVar& var = scope->locals[i];
				Scope::LocalVarCompiled& var_scope = scope->function->locals_compiled[var.index];
				var_scope.cached_name_index = cacheString(var.name);
				var_scope.start_code_pos = start_code_pos;
				var_scope.end_code_pos = getOpcodePos();
			}
			break;
		}

	case EXP_TYPE_IF:
		{
			OS_ASSERT(exp->list.count == 2 || exp->list.count == 3);
			bool inverse;
			Expression * exp_compare;
			if(exp->list[0]->type == EXP_TYPE_LOGIC_NOT){
				OS_ASSERT(exp->list[0]->list.count == 1);
				exp_compare = exp->list[0]->list[0];
				inverse = true;
			}else{
				OS_ASSERT(exp->list.count == 1);
				exp_compare = exp->list[0];
				inverse = false;
			}
			int opcode;
			OS_ASSERT(exp_compare->slots.a > 0 && exp_compare->slots.b > 0);
			switch(exp_compare->type){
			case EXP_TYPE_LOGIC_PTR_EQ:
			case EXP_TYPE_LOGIC_PTR_NE:
				if(!writeOpcodes(scope, exp_compare->list)){
					return false;
				}
				opcode = Program::OP_LOGIC_PTR_EQ;
				inverse ^= exp_compare->type != EXP_TYPE_LOGIC_PTR_EQ;
				break;

			case EXP_TYPE_LOGIC_EQ:
			case EXP_TYPE_LOGIC_NE:
				if(!writeOpcodes(scope, exp_compare->list)){
					return false;
				}
				opcode = Program::OP_LOGIC_EQ;
				inverse ^= exp_compare->type != EXP_TYPE_LOGIC_EQ;
				break;

			case EXP_TYPE_LOGIC_LE:
			case EXP_TYPE_LOGIC_GREATER:
				if(!writeOpcodes(scope, exp_compare->list)){
					return false;
				}
				opcode = Program::OP_LOGIC_GREATER;
				inverse ^= exp_compare->type != EXP_TYPE_LOGIC_GREATER;
				break;

			case EXP_TYPE_LOGIC_GE:
			case EXP_TYPE_LOGIC_LESS:
				if(!writeOpcodes(scope, exp_compare->list)){
					return false;
				}
				opcode = Program::OP_LOGIC_GE;
				inverse ^= exp_compare->type != EXP_TYPE_LOGIC_GE;
				break;

			default:
				if(!writeOpcodes(scope, exp_compare)){
					return false;
				}
				opcode = Program::OP_LOGIC_BOOL;
				break;
			}
			writeDebugInfo(exp);
			int if_jump_pos = writeOpcode(opcode, exp_compare->slots.b, inverse, 0);
			if(!writeOpcodes(scope, exp->list[1])){
				return false;
			}
			int if_jump_to = getOpcodePos();
			if(exp->list.count == 3 && exp->list[2]->list.count > 0){
				int jump_pos = writeOpcode(Program::OP_JUMP);
				
				if_jump_to = getOpcodePos();
				if(!writeOpcodes(scope, exp->list[2])){
					return false;
				}
				fixJumpOpcode(getOpcodePos() - jump_pos - 1, jump_pos, Program::OP_JUMP);
			}
			fixJumpOpcode(if_jump_to - if_jump_pos - 1, if_jump_pos, opcode);
			break;
		}
	}
#endif
	return false;
}

#if 0
void OS::Core::Compiler::writeJumpOpcodeOld(int offs)
{
	offs += 3;
	if((int)(OS_INT8)offs == offs){
		prog_opcodes_old->writeByte(Program::OP_JUMP_1);
		prog_opcodes_old->writeInt8(offs);
		prog_opcodes_old->writeInt8(0);
		prog_opcodes_old->writeInt16(0);
		return;
	}
	if((int)(OS_INT16)offs == offs){
		prog_opcodes_old->writeByte(Program::OP_JUMP_2);
		prog_opcodes_old->writeInt16(offs);
		prog_opcodes_old->writeInt16(0);
		return;
	}
	prog_opcodes_old->writeByte(Program::OP_JUMP_4);
	prog_opcodes_old->writeInt32(offs);
}

void OS::Core::Compiler::fixJumpOpcodeOld(StreamWriter * writer, int offs, int pos)
{
	fixJumpOpcodeOld(writer, offs, pos, Program::OP_JUMP_4);
}

void OS::Core::Compiler::fixJumpOpcodeOld(StreamWriter * writer, int offs, int pos, int opcode)
{
	struct Lib
	{
		static int getFastOpcode(int opcode, int type)
		{
			OS_ASSERT(type >= 1 && type <= 3);
			switch(opcode){
			case Program::OP_JUMP_4:
			case Program::OP_IF_JUMP_4:
			case Program::OP_IF_NOT_JUMP_4:
			case Program::OP_LOGIC_AND_4:
			case Program::OP_LOGIC_OR_4:
				return opcode - 3 + type;
			}
			OS_ASSERT(false);
			return Program::OP_UNKNOWN;
		}
	};
	offs += 4;
	if((int)(OS_INT8)offs == offs){
		writer->writeByteAtPos(Lib::getFastOpcode(opcode, 1), pos);
		writer->writeInt8AtPos(offs, pos+1);
		return;
	}
	if((int)(OS_INT16)offs == offs){
		writer->writeByteAtPos(Lib::getFastOpcode(opcode, 2), pos);
		writer->writeInt16AtPos(offs, pos+1);
		return;
	}
	writer->writeByteAtPos(Lib::getFastOpcode(opcode, 3), pos);
	writer->writeInt32AtPos(offs, pos+1);
}

bool OS::Core::Compiler::writeOpcodesOld(Scope * scope, ExpressionList& list)
{
	for(int i = 0; i < list.count; i++){
		if(!writeOpcodesOld(scope, list[i])){
			return false;
		}
	}
	return true;
}

bool OS::Core::Compiler::writeOpcodesOld(Scope * scope, Expression * exp)
{
	int i;
	// writeDebugInfo(exp);
	switch(exp->type){
	default:
		{
			ExpressionType exp_type = exp->type;
			OS_ASSERT(false);
			break;
		}

	case EXP_TYPE_NOP:
		break;

	case EXP_TYPE_NEW_LOCAL_VAR:
		break;

	case EXP_TYPE_VALUE:
	case EXP_TYPE_CODE_LIST:
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		break;

	case EXP_TYPE_CONST_NUMBER:
		{
			OS_ASSERT(exp->list.count == 0);
			writeDebugInfo(exp);
			OS_NUMBER number = (OS_NUMBER)exp->token->getFloat();
			if(number == 1.0f){
				prog_opcodes_old->writeByte(Program::OP_PUSH_ONE);
			}else{
				int i = cacheNumber(number);
				if(i <= 255){
					prog_opcodes_old->writeByte(Program::OP_PUSH_NUMBER_1);
					prog_opcodes_old->writeByte(i);
				}else{
					prog_opcodes_old->writeByte(Program::OP_PUSH_NUMBER_BY_AUTO_INDEX);
					prog_opcodes_old->writeUVariable(i);
				}
			}
			break;
		}

	case EXP_TYPE_CONST_STRING:
		{
			OS_ASSERT(exp->list.count == 0);
			writeDebugInfo(exp);
			int i = cacheString(exp->token->str);
			if(i <= 255){
				prog_opcodes_old->writeByte(Program::OP_PUSH_STRING_1);
				prog_opcodes_old->writeByte(i);
			}else{
				prog_opcodes_old->writeByte(Program::OP_PUSH_STRING_BY_AUTO_INDEX);
				prog_opcodes_old->writeUVariable(i);
			}
			break;
		}

	case EXP_TYPE_CONST_NULL:
		OS_ASSERT(exp->list.count == 0);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_PUSH_NULL);
		break;

	case EXP_TYPE_CONST_TRUE:
		OS_ASSERT(exp->list.count == 0);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_PUSH_TRUE);
		break;

	case EXP_TYPE_CONST_FALSE:
		OS_ASSERT(exp->list.count == 0);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_PUSH_FALSE);
		break;

	case EXP_TYPE_FUNCTION:
		{
			Scope * scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(scope);
			writeDebugInfo(exp);
			prog_opcodes_old->writeByte(Program::OP_PUSH_FUNCTION);

			int prog_func_index = scope->prog_func_index; // prog_functions.indexOf(scope);
			OS_ASSERT(prog_func_index >= 0);
			prog_opcodes_old->writeUVariable(prog_func_index);

			allocator->vectorReserveCapacity(scope->locals_compiled, scope->num_locals OS_DBG_FILEPOS);
			scope->locals_compiled.count = scope->num_locals;

			scope->opcodes_pos = prog_opcodes_old->getPos();
			if(!writeOpcodesOld(scope, exp->list)){
				return false;
			}
			prog_opcodes_old->writeByte(Program::OP_RETURN_AUTO);
			scope->opcodes_size = prog_opcodes_old->getPos() - scope->opcodes_pos;

			for(i = 0; i < scope->locals.count; i++){
				Scope::LocalVar& var = scope->locals[i];
				Scope::LocalVarCompiled& var_scope = scope->locals_compiled[var.index];
				var_scope.cached_name_index = cacheString(var.name);
				var_scope.start_code_pos = scope->opcodes_pos;
				var_scope.end_code_pos = prog_opcodes_old->getPos();
			}
			break;
		}

	case EXP_TYPE_SCOPE:
	case EXP_TYPE_LOOP_SCOPE:
		{
			Scope * scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(scope);
			int start_code_pos = prog_opcodes_old->getPos();
			if(!writeOpcodesOld(scope, exp->list)){
				return false;
			}
			if(exp->type == EXP_TYPE_LOOP_SCOPE){
				// prog_opcodes_old->writeByte(Program::OP_JUMP);
				// prog_opcodes_old->writeInt32(start_code_pos - prog_opcodes_old->getPos() - sizeof(OS_INT32));
				writeJumpOpcodeOld(start_code_pos - prog_opcodes_old->getPos() - sizeof(OS_INT32));

				scope->fixLoopBreaks(this, start_code_pos, prog_opcodes_old->getPos(), prog_opcodes_old);
			}else{
				OS_ASSERT(scope->loop_breaks.count == 0);
			}
			for(i = 0; i < scope->locals.count; i++){
				Scope::LocalVar& var = scope->locals[i];
				Scope::LocalVarCompiled& var_scope = scope->function->locals_compiled[var.index];
				var_scope.cached_name_index = cacheString(var.name);
				var_scope.start_code_pos = start_code_pos;
				var_scope.end_code_pos = prog_opcodes_old->getPos();
			}
			break;
		}

	case EXP_TYPE_IF:
		{
			OS_ASSERT(exp->list.count == 2 || exp->list.count == 3);
			int if_opcode;
			if(exp->list[0]->type == EXP_TYPE_LOGIC_NOT){
				OS_ASSERT(exp->list[0]->list.count == 1);
				if(!writeOpcodesOld(scope, exp->list[0]->list)){
					return false;
				}
				if_opcode = Program::OP_IF_JUMP_4;
			}else{
				if(!writeOpcodesOld(scope, exp->list[0])){
					return false;
				}
				if_opcode = Program::OP_IF_NOT_JUMP_4;
			}
			writeDebugInfo(exp);
			
			int if_jump_pos = prog_opcodes_old->getPos();
			prog_opcodes_old->writeByte(if_opcode);
			prog_opcodes_old->writeInt32(0);

			if(!writeOpcodesOld(scope, exp->list[1])){
				return false;
			}

			int if_jump_to = prog_opcodes_old->getPos();
			if(exp->list.count == 3 && exp->list[2]->list.count > 0){
				int jump_pos = prog_opcodes_old->getPos();
				prog_opcodes_old->writeByte(Program::OP_JUMP_4);
				prog_opcodes_old->writeInt32(0);

				if_jump_to = prog_opcodes_old->getPos();
				if(!writeOpcodesOld(scope, exp->list[2])){
					return false;
				}
				// prog_opcodes_old->writeInt32AtPos(prog_opcodes_old->getPos() - jump_pos - sizeof(OS_BYTE)*5, jump_pos);
				fixJumpOpcodeOld(prog_opcodes_old, prog_opcodes_old->getPos() - jump_pos - sizeof(OS_BYTE)*5, jump_pos, Program::OP_JUMP_4);
			}
			// prog_opcodes_old->writeInt32AtPos(if_jump_to - if_jump_pos - sizeof(OS_BYTE)*5, if_jump_pos);
			fixJumpOpcodeOld(prog_opcodes_old, if_jump_to - if_jump_pos - sizeof(OS_BYTE)*5, if_jump_pos, if_opcode);
			break;
		}

	case EXP_TYPE_QUESTION:
		{
			OS_ASSERT(exp->list.count == 3);
			int if_opcode;
			if(exp->list[0]->type == EXP_TYPE_LOGIC_NOT){
				OS_ASSERT(exp->list[0]->list.count == 1);
				if(!writeOpcodesOld(scope, exp->list[0]->list)){
					return false;
				}
				if_opcode = Program::OP_IF_JUMP_4;
			}else{
				if(!writeOpcodesOld(scope, exp->list[0])){
					return false;
				}
				if_opcode = Program::OP_IF_NOT_JUMP_4;
			}
			writeDebugInfo(exp);
			
			int if_jump_pos = prog_opcodes_old->getPos();
			prog_opcodes_old->writeByte(if_opcode);
			prog_opcodes_old->writeInt32(0);

			if(!writeOpcodesOld(scope, exp->list[1])){
				return false;
			}

			int jump_pos = prog_opcodes_old->getPos();
			prog_opcodes_old->writeByte(Program::OP_JUMP_4);
			prog_opcodes_old->writeInt32(0);

			int if_jump_to = prog_opcodes_old->getPos();
			if(!writeOpcodesOld(scope, exp->list[2])){
				return false;
			}
			// prog_opcodes_old->writeInt32AtPos(prog_opcodes_old->getPos() - jump_pos - sizeof(OS_BYTE)*5, jump_pos);
			fixJumpOpcodeOld(prog_opcodes_old, prog_opcodes_old->getPos() - jump_pos - sizeof(OS_BYTE)*5, jump_pos, Program::OP_JUMP_4);

			// prog_opcodes_old->writeInt32AtPos(if_jump_to - if_jump_pos - sizeof(OS_BYTE)*5, if_jump_pos);
			fixJumpOpcodeOld(prog_opcodes_old, if_jump_to - if_jump_pos - sizeof(OS_BYTE)*5, if_jump_pos, if_opcode);
			break;
		}

	case EXP_TYPE_LOGIC_AND: // &&
	case EXP_TYPE_LOGIC_OR:  // ||
		{
			OS_ASSERT(exp->list.count == 2);
			if(!writeOpcodesOld(scope, exp->list[0])){
				return false;
			}
			writeDebugInfo(exp);

			int opcode = Program::getOpcodeType(exp->type);
			int op_jump_pos = prog_opcodes_old->getPos();
			prog_opcodes_old->writeByte(opcode);
			prog_opcodes_old->writeInt32(0);

			if(!writeOpcodesOld(scope, exp->list[1])){
				return false;
			}

			int op_jump_to = prog_opcodes_old->getPos();
			// prog_opcodes_old->writeInt32AtPos(op_jump_to - op_jump_pos - sizeof(OS_BYTE)*5, op_jump_pos);
			fixJumpOpcodeOld(prog_opcodes_old, op_jump_to - op_jump_pos - sizeof(OS_BYTE)*5, op_jump_pos, opcode);
			break;
		}

	case EXP_TYPE_EXTENDS:
		OS_ASSERT(exp->list.count == 2);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_EXTENDS);
		break;

	/*
	case EXP_TYPE_CLONE:
		OS_ASSERT(exp->list.count == 1);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_CLONE);
		break;
	*/

	case EXP_TYPE_DELETE:
		OS_ASSERT(exp->list.count == 2);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_DELETE_PROP);
		break;

	case EXP_TYPE_ARRAY:
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_PUSH_NEW_ARRAY);
		prog_opcodes_old->writeByte(exp->list.count > 255 ? 256 : exp->list.count);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		break;

	case EXP_TYPE_OBJECT:
		// OS_ASSERT(exp->list.count >= 0);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_PUSH_NEW_OBJECT);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		break;

	case EXP_TYPE_OBJECT_SET_BY_AUTO_INDEX:
		OS_ASSERT(exp->list.count == 1);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_OBJECT_SET_BY_AUTO_INDEX);
		break;

	case EXP_TYPE_OBJECT_SET_BY_EXP:
		OS_ASSERT(exp->list.count == 2);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_OBJECT_SET_BY_EXP);
		break;

	case EXP_TYPE_OBJECT_SET_BY_INDEX:
		OS_ASSERT(exp->list.count == 1);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_OBJECT_SET_BY_INDEX);
		// prog_opcodes_old->writeInt64(exp->token->getInt());
		prog_opcodes_old->writeUVariable(cacheNumber((OS_NUMBER)exp->token->getFloat()));
		break;

	case EXP_TYPE_OBJECT_SET_BY_NAME:
		OS_ASSERT(exp->list.count == 1);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_OBJECT_SET_BY_NAME);
		prog_opcodes_old->writeUVariable(cacheString(exp->token->str));
		break;

	case EXP_TYPE_GET_ENV_VAR:
		OS_ASSERT(exp->list.count == 0);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_PUSH_ENV_VAR);
		prog_opcodes_old->writeUVariable(cacheString(exp->token->str));
		break;

	case EXP_TYPE_GET_ENV_VAR_AUTO_CREATE:
		OS_ASSERT(exp->list.count == 0);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_PUSH_ENV_VAR_AUTO_CREATE);
		prog_opcodes_old->writeUVariable(cacheString(exp->token->str));
		break;

	case EXP_TYPE_SET_ENV_VAR:
		OS_ASSERT(exp->list.count > 0);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_SET_ENV_VAR);
		prog_opcodes_old->writeUVariable(cacheString(exp->token->str));
		break;

	case EXP_TYPE_GET_THIS:
	case EXP_TYPE_GET_ARGUMENTS:
	case EXP_TYPE_GET_REST_ARGUMENTS:
		OS_ASSERT(exp->list.count == 0);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::getOpcodeType(exp->type));
		break;

	case EXP_TYPE_GET_LOCAL_VAR:
		OS_ASSERT(exp->list.count == 0);
		if(!exp->local_var.up_count){
			writeDebugInfo(exp);
			if(exp->local_var.index <= 255){
				prog_opcodes_old->writeByte(Program::OP_PUSH_LOCAL_VAR_1);
				prog_opcodes_old->writeByte(exp->local_var.index);
			}else{
				prog_opcodes_old->writeByte(Program::OP_PUSH_LOCAL_VAR_BY_AUTO_INDEX);
				prog_opcodes_old->writeUVariable(exp->local_var.index);
			}
		}else{
			writeDebugInfo(exp);
			prog_opcodes_old->writeByte(Program::OP_PUSH_UP_LOCAL_VAR);
			prog_opcodes_old->writeUVariable(exp->local_var.index);
			prog_opcodes_old->writeByte(exp->local_var.up_count);
		}
		break;

	case EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE:
		OS_ASSERT(exp->list.count == 0);
		if(!exp->local_var.up_count){
			writeDebugInfo(exp);
			prog_opcodes_old->writeByte(Program::OP_PUSH_LOCAL_VAR_AUTO_CREATE);
			prog_opcodes_old->writeUVariable(exp->local_var.index);
		}else{
			writeDebugInfo(exp);
			prog_opcodes_old->writeByte(Program::OP_PUSH_UP_LOCAL_VAR_AUTO_CREATE);
			prog_opcodes_old->writeUVariable(exp->local_var.index);
			prog_opcodes_old->writeByte(exp->local_var.up_count);
		}
		break;

	case EXP_TYPE_SET_LOCAL_VAR:
		OS_ASSERT(exp->list.count > 0);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		if(!exp->local_var.up_count){
			if(exp->local_var.index <= 255){
				prog_opcodes_old->writeByte(Program::OP_SET_LOCAL_VAR_1);
				prog_opcodes_old->writeByte(exp->local_var.index);
			}else{
				prog_opcodes_old->writeByte(Program::OP_SET_LOCAL_VAR);
				prog_opcodes_old->writeUVariable(exp->local_var.index);
			}
		}else{
			prog_opcodes_old->writeByte(Program::OP_SET_UP_LOCAL_VAR);
			prog_opcodes_old->writeUVariable(exp->local_var.index);
			prog_opcodes_old->writeByte(exp->local_var.up_count);
		}
		break;

	case EXP_TYPE_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCALS:
		{
			OS_ASSERT(exp->list.count == 1);
			OS_ASSERT(!exp->local_var.up_count);
			OS_ASSERT(exp->list[0]->type == EXP_TYPE_BIN_OPERATOR_BY_LOCALS);
			writeDebugInfo(exp);
			prog_opcodes_old->writeByte(Program::OP_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCALS);
			Expression * exp_binary = exp->list[0]->list[0];
			Expression * exp1 = exp_binary->list[0];
			Expression * exp2 = exp_binary->list[1];
			prog_opcodes_old->writeByte(Program::getOpcodeType(exp_binary->type));
			prog_opcodes_old->writeByte(exp1->local_var.index);
			prog_opcodes_old->writeByte(exp2->local_var.index);
			prog_opcodes_old->writeUVariable(exp->local_var.index);
			break;
		}

	case EXP_TYPE_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCAL_AND_NUMBER:
		{
			OS_ASSERT(exp->list.count == 1);
			OS_ASSERT(!exp->local_var.up_count);
			OS_ASSERT(exp->list[0]->type == EXP_TYPE_BIN_OPERATOR_BY_LOCAL_AND_NUMBER);
			writeDebugInfo(exp);
			Expression * exp_binary = exp->list[0]->list[0];
			Expression * exp1 = exp_binary->list[0];
			Expression * exp2 = exp_binary->list[1];
			int number_index = cacheNumber((OS_NUMBER)exp2->token->getFloat());
			if(number_index <= 255 && exp->local_var.index <= 255){
				prog_opcodes_old->writeByte(Program::OP_SET_LOCAL_VAR_1_BY_BIN_OPERATOR_LOCAL_AND_NUMBER);
				prog_opcodes_old->writeByte(Program::getOpcodeType(exp_binary->type));
				prog_opcodes_old->writeByte(exp1->local_var.index);
				prog_opcodes_old->writeByte(number_index);
				prog_opcodes_old->writeByte(exp->local_var.index);
			}else{
				prog_opcodes_old->writeByte(Program::OP_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCAL_AND_NUMBER);
				prog_opcodes_old->writeByte(Program::getOpcodeType(exp_binary->type));
				prog_opcodes_old->writeByte(exp1->local_var.index);
				prog_opcodes_old->writeUVariable(number_index);
				prog_opcodes_old->writeUVariable(exp->local_var.index);
			}
			break;
		}

	case EXP_TYPE_CALL:
	case EXP_TYPE_CALL_AUTO_PARAM:
		{
			OS_ASSERT(exp->list.count == 2);
			OS_ASSERT(exp->list[1]->type == EXP_TYPE_PARAMS);

			bool is_super_call = exp->list[0]->type == EXP_TYPE_SUPER;
			if(is_super_call){
				prog_opcodes_old->writeByte(Program::OP_PUSH_NULL); // func
			}else if(!writeOpcodesOld(scope, exp->list[0])){
				return false;
			}
			// writeDebugInfo(exp);
			prog_opcodes_old->writeByte(Program::OP_PUSH_NULL); // this
			if(!writeOpcodesOld(scope, exp->list[1])){
				return false;
			}
			writeDebugInfo(exp);
			prog_opcodes_old->writeByte(is_super_call ? Program::OP_SUPER_CALL : Program::getOpcodeType(exp->type));
			prog_opcodes_old->writeByte(exp->list[1]->ret_values); // params number
			prog_opcodes_old->writeByte(exp->ret_values);
			break;
		}

	case EXP_TYPE_TAIL_CALL:
		OS_ASSERT(exp->list.count == 2);
		OS_ASSERT(exp->list[1]->type == EXP_TYPE_PARAMS);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::getOpcodeType(exp->type));
		prog_opcodes_old->writeByte(exp->list[1]->ret_values); // params number
		break;

		// case EXP_TYPE_GET_DIM:
	case EXP_TYPE_CALL_METHOD:
		OS_ASSERT(exp->list.count == 2);
		OS_ASSERT(exp->list[1]->type == EXP_TYPE_PARAMS);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::getOpcodeType(exp->type));
		prog_opcodes_old->writeByte(exp->list[1]->ret_values-1); // params number
		prog_opcodes_old->writeByte(exp->ret_values);
		break;

	case EXP_TYPE_TAIL_CALL_METHOD:
		OS_ASSERT(exp->list.count == 2);
		OS_ASSERT(exp->list[1]->type == EXP_TYPE_PARAMS);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::getOpcodeType(exp->type));
		prog_opcodes_old->writeByte(exp->list[1]->ret_values-1); // params number
		break;

	case EXP_TYPE_GET_PROPERTY:
		OS_ASSERT(exp->list.count == 2);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_GET_PROPERTY);
		prog_opcodes_old->writeByte(exp->ret_values);
		break;

	case EXP_TYPE_GET_THIS_PROPERTY_BY_STRING:
		{
			OS_ASSERT(exp->list.count == 2);
			OS_ASSERT(exp->list[0]->type == EXP_TYPE_GET_THIS);
			OS_ASSERT(exp->list[1]->type == EXP_TYPE_CONST_STRING);
			writeDebugInfo(exp);
			prog_opcodes_old->writeByte(Program::OP_GET_THIS_PROPERTY_BY_STRING);
			prog_opcodes_old->writeByte(exp->ret_values);
			prog_opcodes_old->writeUVariable(cacheString(exp->list[1]->token->str));
			break;
		}

	case EXP_TYPE_GET_PROPERTY_BY_LOCALS:
		OS_ASSERT(exp->list.count == 2);
		OS_ASSERT(exp->list[0]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[0]->local_var.up_count);
		OS_ASSERT(exp->list[1]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[1]->local_var.up_count);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_GET_PROPERTY_BY_LOCALS);
		prog_opcodes_old->writeByte(exp->ret_values);
		prog_opcodes_old->writeByte(exp->list[0]->local_var.index);
		prog_opcodes_old->writeByte(exp->list[1]->local_var.index);
		break;

	case EXP_TYPE_GET_PROPERTY_BY_LOCAL_AND_NUMBER:
		OS_ASSERT(exp->list.count == 2);
		OS_ASSERT(exp->list[0]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[0]->local_var.up_count);
		OS_ASSERT(exp->list[1]->type == EXP_TYPE_CONST_NUMBER);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_GET_PROPERTY_BY_LOCAL_AND_NUMBER);
		prog_opcodes_old->writeByte(exp->ret_values);
		prog_opcodes_old->writeByte(exp->list[0]->local_var.index);
		prog_opcodes_old->writeUVariable(cacheNumber((OS_NUMBER)exp->list[1]->token->getFloat()));
		break;

	case EXP_TYPE_GET_PROPERTY_AUTO_CREATE:
		OS_ASSERT(exp->list.count == 2);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_GET_PROPERTY_AUTO_CREATE);
		prog_opcodes_old->writeByte(exp->ret_values);
		break;

	case EXP_TYPE_SET_PROPERTY:
		OS_ASSERT(exp->list.count == 3);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_SET_PROPERTY);
		break;

	case EXP_TYPE_SET_PROPERTY_BY_LOCALS_AUTO_CREATE:
		OS_ASSERT(exp->list.count == 3);
		OS_ASSERT(exp->list[1]->type == EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE && !exp->list[1]->local_var.up_count);
		OS_ASSERT(exp->list[2]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[2]->local_var.up_count);
		writeOpcodesOld(scope, exp->list[0]);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_SET_PROPERTY_BY_LOCALS_AUTO_CREATE);
		prog_opcodes_old->writeByte(exp->list[1]->local_var.index);
		prog_opcodes_old->writeByte(exp->list[2]->local_var.index);
		break;

	case EXP_TYPE_GET_SET_PROPERTY_BY_LOCALS_AUTO_CREATE:
		OS_ASSERT(exp->list.count == 3);
		OS_ASSERT(exp->list[0]->type == EXP_TYPE_GET_PROPERTY_BY_LOCALS && exp->list[0]->list.count == 2);
		OS_ASSERT(exp->list[0]->list[0]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[0]->list[0]->local_var.up_count);
		OS_ASSERT(exp->list[0]->list[1]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[0]->list[1]->local_var.up_count);
		OS_ASSERT(exp->list[1]->type == EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE && !exp->list[1]->local_var.up_count);
		OS_ASSERT(exp->list[2]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[2]->local_var.up_count);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_GET_SET_PROPERTY_BY_LOCALS_AUTO_CREATE);
		prog_opcodes_old->writeByte(exp->list[0]->list[0]->local_var.index);
		prog_opcodes_old->writeByte(exp->list[0]->list[1]->local_var.index);
		prog_opcodes_old->writeByte(exp->list[1]->local_var.index);
		prog_opcodes_old->writeByte(exp->list[2]->local_var.index);
		break;

	case EXP_TYPE_SET_DIM:
		OS_ASSERT(exp->list.count == 3);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_SET_DIM);
		prog_opcodes_old->writeByte(exp->list[2]->list.count); // params
		break;

	case EXP_TYPE_PARAMS:
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		break;

	case EXP_TYPE_RETURN:
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_RETURN);
		prog_opcodes_old->writeByte(exp->ret_values);
		break;

	case EXP_TYPE_BREAK:
		OS_ASSERT(exp->list.count == 0);
		writeDebugInfo(exp);
		scope->addLoopBreak(prog_opcodes_old->getPos(), Scope::LOOP_BREAK);
		prog_opcodes_old->writeByte(Program::OP_JUMP_4);
		prog_opcodes_old->writeInt32(0);
		break;

	case EXP_TYPE_CONTINUE:
		OS_ASSERT(exp->list.count == 0);
		writeDebugInfo(exp);
		scope->addLoopBreak(prog_opcodes_old->getPos(), Scope::LOOP_CONTINUE);
		prog_opcodes_old->writeByte(Program::OP_JUMP_4);
		prog_opcodes_old->writeInt32(0);
		break;

	case EXP_TYPE_DEBUGGER:
		{
			OS_ASSERT(exp->list.count == 0);
			prog_opcodes_old->writeByte(Program::OP_DEBUGGER);
			prog_opcodes_old->writeUVariable(exp->token->line + 1);
			prog_opcodes_old->writeUVariable(exp->token->pos + 1);
			prog_opcodes_old->writeUVariable(OS_DEBUGGER_SAVE_NUM_LINES);
			Core::String empty(allocator);
			for(int i = 0; i < OS_DEBUGGER_SAVE_NUM_LINES; i++){
				int j = exp->token->line - OS_DEBUGGER_SAVE_NUM_LINES/2 + i;
				if(j >= 0 && j < exp->token->text_data->lines.count){
					prog_opcodes_old->writeUVariable(cacheString(exp->token->text_data->lines[j]));
				}else{
					prog_opcodes_old->writeUVariable(cacheString(empty));
				}
			}
			break;
		}

	case EXP_TYPE_DEBUG_LOCALS:
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		break;

	case EXP_TYPE_POP_VALUE:
		OS_ASSERT(exp->list.count == 1);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		// writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_POP);
		break;

	case EXP_TYPE_SUPER:
		OS_ASSERT(exp->list.count == 0);
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::OP_SUPER);
		break;
	/*
	case EXP_TYPE_TYPE_OF:
	case EXP_TYPE_VALUE_OF:
	case EXP_TYPE_NUMBER_OF:
	case EXP_TYPE_STRING_OF:
	case EXP_TYPE_ARRAY_OF:
	case EXP_TYPE_OBJECT_OF:
	case EXP_TYPE_USERDATA_OF:
	case EXP_TYPE_FUNCTION_OF:
	*/
	case EXP_TYPE_LOGIC_BOOL:
	case EXP_TYPE_LOGIC_NOT:
	case EXP_TYPE_BIT_NOT:
	case EXP_TYPE_PLUS:
	case EXP_TYPE_NEG:
	case EXP_TYPE_LENGTH:
		OS_ASSERT(exp->list.count == 1);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::getOpcodeType(exp->type));
		break;

	case EXP_TYPE_CONCAT:
	case EXP_TYPE_IN:
	case EXP_TYPE_ISPROTOTYPEOF:
	case EXP_TYPE_IS:

		// case EXP_TYPE_LOGIC_AND:
		// case EXP_TYPE_LOGIC_OR:
	case EXP_TYPE_LOGIC_PTR_EQ:
	case EXP_TYPE_LOGIC_PTR_NE:
	case EXP_TYPE_LOGIC_EQ:
	case EXP_TYPE_LOGIC_NE:
	case EXP_TYPE_LOGIC_GE:
	case EXP_TYPE_LOGIC_LE:
	case EXP_TYPE_LOGIC_GREATER:
	case EXP_TYPE_LOGIC_LESS:

	case EXP_TYPE_BIT_AND:
	case EXP_TYPE_BIT_OR:
	case EXP_TYPE_BIT_XOR:

	case EXP_TYPE_ADD:
	case EXP_TYPE_SUB:
	case EXP_TYPE_MUL:
	case EXP_TYPE_DIV:
	case EXP_TYPE_MOD:
	case EXP_TYPE_LSHIFT:
	case EXP_TYPE_RSHIFT:
	case EXP_TYPE_POW:
		OS_ASSERT(exp->list.count == 2);
		if(!writeOpcodesOld(scope, exp->list)){
			return false;
		}
		writeDebugInfo(exp);
		prog_opcodes_old->writeByte(Program::getOpcodeType(exp->type));
		break;

	case EXP_TYPE_BIN_OPERATOR_BY_LOCALS:
		{
			OS_ASSERT(exp->list.count == 1);
			OS_ASSERT(exp->list[0]->isBinaryOperator());
			OS_ASSERT(exp->list[0]->list[0]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[0]->list[0]->local_var.up_count);
			OS_ASSERT(exp->list[0]->list[1]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[0]->list[1]->local_var.up_count);
			writeDebugInfo(exp);
			prog_opcodes_old->writeByte(Program::OP_BIN_OPERATOR_BY_LOCALS);
			Expression * exp_binary = exp->list[0];
			Expression * exp1 = exp_binary->list[0];
			Expression * exp2 = exp_binary->list[1];
			prog_opcodes_old->writeByte(Program::getOpcodeType(exp_binary->type));
			prog_opcodes_old->writeByte(exp1->local_var.index);
			prog_opcodes_old->writeByte(exp2->local_var.index);
			break;
		}

	case EXP_TYPE_BIN_OPERATOR_BY_LOCAL_AND_NUMBER:
		{
			OS_ASSERT(exp->list.count == 1);
			OS_ASSERT(exp->list[0]->isBinaryOperator());
			OS_ASSERT(exp->list[0]->list[0]->type == EXP_TYPE_GET_LOCAL_VAR && !exp->list[0]->list[0]->local_var.up_count);
			OS_ASSERT(exp->list[0]->list[1]->type == EXP_TYPE_CONST_NUMBER);
			writeDebugInfo(exp);
			prog_opcodes_old->writeByte(Program::OP_BIN_OPERATOR_BY_LOCAL_AND_NUMBER);
			Expression * exp_binary = exp->list[0];
			Expression * exp1 = exp_binary->list[0];
			Expression * exp2 = exp_binary->list[1];
			prog_opcodes_old->writeByte(Program::getOpcodeType(exp_binary->type));
			prog_opcodes_old->writeByte(exp1->local_var.index);
			prog_opcodes_old->writeUVariable(cacheNumber((OS_NUMBER)exp2->token->getFloat()));
			break;
		}
	}
	return true;
}
#endif // 0

// =====================================================================

OS::Core::Compiler::Scope::Scope(Scope * p_parent, ExpressionType type, TokenData * token): Expression(type, token)
{
	OS_ASSERT(type == EXP_TYPE_FUNCTION || type == EXP_TYPE_SCOPE || type == EXP_TYPE_LOOP_SCOPE);
	parent = p_parent;
	function = type == EXP_TYPE_FUNCTION ? this : parent->function;
	num_params = 0;
	num_locals = 0;
	max_up_count = 0;
	func_depth = 0;
	func_index = 0;
	num_local_funcs = 0;
	prog_func_index = -1;
	parser_started = false;
	stack_size = 0;
	stack_cur_size = 0;
}

OS::Core::Compiler::Scope::~Scope()
{
	getAllocator()->vectorClear(locals);
	// getAllocator()->vectorClear(func_locals);
	getAllocator()->vectorClear(locals_compiled);
	getAllocator()->vectorClear(loop_breaks);
}

OS::Core::Compiler::Scope::LocalVar::LocalVar(const String& p_name, int p_index): name(p_name)
{
	index = p_index;
}

OS::Core::Compiler::Scope::LocalVarCompiled::LocalVarCompiled()
{
	cached_name_index = -1;
	start_code_pos = -1;
	end_code_pos = -1;
}

bool OS::Core::Compiler::Scope::addLoopBreak(int pos, ELoopBreakType type)
{
	Scope * scope = this;
	for(; scope; scope = scope->parent){
		if(scope->type == EXP_TYPE_LOOP_SCOPE){
			break;
		}
	}
	if(!scope){
		return false;
	}
	LoopBreak loop_break;
	loop_break.pos = pos;
	loop_break.type = type;
	getAllocator()->vectorAddItem(scope->loop_breaks, loop_break OS_DBG_FILEPOS);
	return true;
}

void OS::Core::Compiler::Scope::fixLoopBreaks(Compiler * compiler, int scope_start_pos, int scope_end_pos)
{
	for(int i = 0; i < loop_breaks.count; i++){
		LoopBreak& loop_break = loop_breaks[i];
		if(loop_break.type == LOOP_BREAK){
			int offs = scope_end_pos - loop_break.pos - sizeof(OS_BYTE)*5;
			// compiler->fixJumpOpcode(offs, loop_break.pos);
		}else{
			int offs = scope_start_pos - loop_break.pos - sizeof(OS_BYTE)*5;
			// compiler->fixJumpOpcode(offs, loop_break.pos);
		}
	}
}

void OS::Core::Compiler::Scope::addStdVars()
{
	Core::Strings * strings = getAllocator()->core->strings;
	// don't change following order
	OS_ASSERT(VAR_THIS == 0 && VAR_ENV == 1);
	addLocalVar(strings->syntax_this);
	addLocalVar(strings->var_env);
#ifdef OS_GLOBAL_VAR_ENABLED
	OS_ASSERT(VAR_GLOBALS == 2);
	addLocalVar(strings->var_globals);
#endif
}

void OS::Core::Compiler::Scope::addLocalVar(const String& name)
{
	OS_ASSERT(function->stack_size == function->num_locals);
	OS_ASSERT(function->stack_cur_size == function->num_locals);
	OS * allocator = getAllocator();
	LocalVar local_var(name, function->num_locals);
	allocator->vectorAddItem(locals, local_var OS_DBG_FILEPOS);
	function->stack_size = function->stack_cur_size = ++function->num_locals;
}

void OS::Core::Compiler::Scope::addLocalVar(const String& name, LocalVarDesc& local_var)
{
	local_var.index = function->num_locals;
	local_var.up_count = 0;
	local_var.type = LOCAL_GENERIC;
	addLocalVar(name);
}

int OS::Core::Compiler::Scope::allocTempVar()
{
	if(++function->stack_cur_size > function->stack_size){
		OS_ASSERT(function->stack_cur_size == function->stack_size+1);
		function->stack_size = function->stack_cur_size;
	}
	return function->stack_cur_size-1;
}

/* int OS::Core::Compiler::Scope::allocTempVar(int a, int b)
{
	if(a > function->num_locals){
		return
	}
} */

void OS::Core::Compiler::Scope::popTempVar(int count)
{
	function->stack_cur_size -= count;
	OS_ASSERT(function->stack_cur_size >= function->num_locals);
}

/*
int OS::Core::Compiler::Scope::newTempVar()
{
	addLocalVar(getAllocator()->core->strings->var_temp_prefix); // String(getAllocator(), OS_TEXT("#")));
	return function->num_locals-1;
}

int OS::Core::Compiler::Scope::newTempVar(int a, int b)
{
	String& temp = getAllocator()->core->strings->var_temp_prefix;
	if(a > 0 && function->func_locals[a].name == temp){
		return a;
	}
	if(b > 0 && function->func_locals[b].name == temp){
		return b;
	}
	return newTempVar();
}
*/

// =====================================================================

OS::Core::Compiler::Compiler(Tokenizer * p_tokenizer)
	: expect_token(p_tokenizer->getAllocator())
{
	allocator = p_tokenizer->getAllocator();
	tokenizer = p_tokenizer;

	error = ERROR_NOTHING;
	error_token = NULL;
	expect_token_type = Tokenizer::NOTHING;

	recent_token = NULL;
	next_token_index = 0;

	recent_printed_text_data = NULL;
	recent_printed_line = 0;

	// prog = NULL;
	prog_strings_table = NULL;
	prog_debug_strings_table = NULL;
	prog_numbers_table = NULL;
	prog_opcodes_old = NULL;
	prog_debug_info = NULL;
	prog_num_debug_infos = 0;
	prog_max_up_count = 0;
}

OS::Core::Compiler::~Compiler()
{
	if(recent_printed_text_data){
		recent_printed_text_data->release();
	}
	if(prog_numbers_table){
		allocator->core->deleteTable(prog_numbers_table);
		prog_numbers_table = NULL;
	}
	if(prog_strings_table){
		allocator->core->deleteTable(prog_strings_table);
		prog_strings_table = NULL;
	}
	if(prog_debug_strings_table){
		allocator->core->deleteTable(prog_debug_strings_table);
		prog_debug_strings_table = NULL;
	}
	allocator->vectorClear(prog_numbers);
	allocator->vectorClear(prog_strings);
	allocator->vectorClear(prog_debug_strings);
	allocator->vectorClear(prog_functions);
	allocator->deleteObj(prog_opcodes_old);
	allocator->deleteObj(prog_debug_info);
	// allocator->deleteObj(tokenizer);
}

bool OS::Core::Compiler::compile()
{
	OS_ASSERT(!prog_opcodes_old && !prog_strings_table && !prog_debug_strings_table && !prog_numbers_table);
	OS_ASSERT(!prog_functions.count && !prog_numbers.count && !prog_strings.count);

	Scope * scope = NULL;
	if(tokenizer->isError()){
		setError(ERROR_SYNTAX, NULL);
	}else if(!readToken()){
		setError(ERROR_EXPECT_TOKEN, recent_token);
	}else{
		scope = expectTextExpression();
	}
	if(scope){
		prog_strings_table = allocator->core->newTable(OS_DBG_FILEPOS_START);
		prog_numbers_table = allocator->core->newTable(OS_DBG_FILEPOS_START);
		
		Expression * exp = postCompileExpression(scope, scope);
		OS_ASSERT(exp->type == EXP_TYPE_FUNCTION);

		prog_opcodes_old = new (malloc(sizeof(MemStreamWriter) OS_DBG_FILEPOS)) MemStreamWriter(allocator);

		OS::String filename(allocator, tokenizer->getTextData()->filename);
		bool is_eval = filename.getDataSize() == 0;

		if(!is_eval && allocator->core->settings.create_debug_opcodes){
			Core::StringBuffer dump(allocator);
			OS_ASSERT(dynamic_cast<Scope*>(exp));
			exp->debugPrint(dump, this, dynamic_cast<Scope*>(exp), 0);
			OS::String dump_filename = allocator->getDebugOpcodesFilename(filename);
			FileStreamWriter(allocator, dump_filename).writeBytes(dump.buf, dump.count * sizeof(OS_CHAR));
		}
		prog_debug_strings_table = allocator->core->newTable(OS_DBG_FILEPOS_START);
		prog_debug_info = new (malloc(sizeof(MemStreamWriter) OS_DBG_FILEPOS)) MemStreamWriter(allocator);

		if(!writeOpcodes(scope, exp)){
			// TODO:
		}

		MemStreamWriter mem_writer(allocator);
		MemStreamWriter debuginfo_mem_writer(allocator);
		saveToStream(&mem_writer, &debuginfo_mem_writer);

		if(!is_eval && allocator->core->settings.create_compiled_file){
			OS::String compiled_filename = allocator->getCompiledFilename(filename);
			FileStreamWriter(allocator, compiled_filename).writeBytes(mem_writer.buffer.buf, mem_writer.buffer.count);
			if(allocator->core->settings.create_debug_info){
				OS::String debug_info_filename = allocator->getDebugInfoFilename(filename);
				FileStreamWriter(allocator, debug_info_filename).writeBytes(debuginfo_mem_writer.buffer.buf, debuginfo_mem_writer.buffer.count);
			}
		}

		Program * prog = new (malloc(sizeof(Program) OS_DBG_FILEPOS)) Program(allocator);
		prog->filename = tokenizer->getTextData()->filename;

		MemStreamReader mem_reader(NULL, mem_writer.buffer.buf, mem_writer.buffer.count);
		MemStreamReader debuginfo_mem_reader(NULL, debuginfo_mem_writer.buffer.buf, debuginfo_mem_writer.buffer.count);
		prog->loadFromStream(&mem_reader, &debuginfo_mem_reader);

		prog->pushStartFunction();
		prog->release();

		allocator->deleteObj(exp);

		return true;
	}else{
		Core::StringBuffer dump(allocator);
		dump += OS_TEXT("Error");
		switch(error){
		default:
			dump += OS_TEXT(" unknown");
			break;

		case ERROR_SYNTAX:
			dump += OS_TEXT(" SYNTAX");
			break;

		case ERROR_NESTED_ROOT_BLOCK:
			dump += OS_TEXT(" NESTED_ROOT_BLOCK");
			break;

		case ERROR_LOCAL_VAL_NOT_DECLARED:
			dump += OS_TEXT(" LOCAL_VAL_NOT_DECLARED");
			break;

		case ERROR_VAR_ALREADY_EXIST:
			dump += OS_TEXT(" VAR_ALREADY_EXIST");
			break;

		case ERROR_VAR_NAME:
			dump += OS_TEXT(" VAR_NAME");
			break;

		case ERROR_EXPECT_TOKEN_TYPE:
			dump += OS_TEXT(" EXPECT_TOKEN_TYPE ");
			dump += Tokenizer::getTokenTypeName(expect_token_type);
			break;

		case ERROR_EXPECT_TOKEN_STR:
			dump += OS_TEXT(" EXPECT_TOKEN_STR ");
			dump += expect_token;
			break;

		case ERROR_EXPECT_TOKEN:
			dump += OS_TEXT(" EXPECT_TOKEN");
			break;

		case ERROR_EXPECT_VALUE:
			dump += OS_TEXT(" EXPECT_VALUE");
			break;

		case ERROR_EXPECT_WRITEABLE:
			dump += OS_TEXT(" EXPECT_WRITEABLE");
			break;

		case ERROR_EXPECT_GET_OR_SET:
			dump += OS_TEXT(" EXPECT_GET_OR_SET");
			break;

		case ERROR_EXPECT_EXPRESSION:
			dump += OS_TEXT(" EXPECT_EXPRESSION");
			break;

		case ERROR_EXPECT_FUNCTION_SCOPE:
			dump += OS_TEXT(" EXPECT_FUNCTION_SCOPE");
			break;

		case ERROR_EXPECT_CODE_SEP_BEFORE_NESTED_BLOCK:
			dump += OS_TEXT(" EXPECT_CODE_SEP_BEFORE_NESTED_BLOCK");
			break;

		case ERROR_EXPECT_SWITCH_SCOPE:
			dump += OS_TEXT(" EXPECT_SWITCH_SCOPE");
			break;

		case ERROR_FINISH_BINARY_OP:
			dump += OS_TEXT(" FINISH_BINARY_OP");
			break;

		case ERROR_FINISH_UNARY_OP:
			dump += OS_TEXT(" FINISH_UNARY_OP");
			break;
		}
		dump += OS_TEXT("\n");
		if(error_token){
			if(error_token->text_data->filename.getDataSize() > 0){
				dump += OS::Core::String::format(allocator, "filename %s\n", error_token->text_data->filename.toChar());
			}
			dump += OS::Core::String::format(allocator, "[%d] %s\n", error_token->line+1, error_token->text_data->lines[error_token->line].toChar());
			dump += OS::Core::String::format(allocator, "pos %d, token: %s\n", error_token->pos+1, error_token->str.toChar());
		}else if(tokenizer->isError()){
			if(tokenizer->getFilename().getDataSize() > 0){
				dump += OS::Core::String::format(allocator, "filename %s\n", tokenizer->getFilename().toChar());
			}
			dump += OS::Core::String::format(allocator, "[%d] %s\n", tokenizer->getErrorLine()+1, tokenizer->getLineString(tokenizer->getErrorLine()).toChar());
			dump += OS::Core::String::format(allocator, "pos %d\n", tokenizer->getErrorPos()+1);
		}
		allocator->printf("%s", dump.toString().toChar());
		// FileStreamWriter(allocator, "test-data/debug-exp-dump.txt").writeBytes(dump.toChar(), dump.getDataSize());

		allocator->pushNull();
	}
	return false;
}

void * OS::Core::Compiler::malloc(int size OS_DBG_FILEPOS_DECL)
{
	return allocator->malloc(size OS_DBG_FILEPOS_PARAM);
}

void OS::Core::Compiler::resetError()
{
	error = ERROR_NOTHING;
	error_token = NULL;
	expect_token_type = Tokenizer::NOTHING;
}

void OS::Core::Compiler::setError(ErrorType value, TokenData * error_token)
{
	OS_ASSERT(!isError());
	error = value;
	this->error_token = error_token;
	expect_token_type = Tokenizer::NOTHING;
}

void OS::Core::Compiler::setError(TokenType expect_token_type, TokenData * error_token)
{
	OS_ASSERT(!isError());
	error = ERROR_EXPECT_TOKEN_TYPE;
	this->error_token = error_token;
	this->expect_token_type = expect_token_type;
}

void OS::Core::Compiler::setError(const String& str, TokenData * error_token)
{
	OS_ASSERT(!isError());
	error = ERROR_EXPECT_TOKEN_STR;
	this->error_token = error_token;
	expect_token_type = Tokenizer::NOTHING;
	expect_token = str;
}

bool OS::Core::Compiler::isError()
{
	return error != ERROR_NOTHING;
}

OS::Core::Compiler::ExpressionType OS::Core::Compiler::getUnaryExpressionType(TokenType token_type)
{
	switch(token_type){
	case Tokenizer::OPERATOR_LENGTH: return EXP_TYPE_LENGTH;
	case Tokenizer::OPERATOR_BIT_NOT: return EXP_TYPE_BIT_NOT;
	case Tokenizer::OPERATOR_ADD: return EXP_TYPE_PLUS;
	case Tokenizer::OPERATOR_SUB: return EXP_TYPE_NEG;
	case Tokenizer::OPERATOR_LOGIC_NOT: return EXP_TYPE_LOGIC_NOT;
	}
	return EXP_TYPE_UNKNOWN;
}

OS::Core::Compiler::ExpressionType OS::Core::Compiler::getExpressionType(TokenType token_type)
{
	switch(token_type){
	case Tokenizer::PARAM_SEPARATOR: return EXP_TYPE_PARAMS;

	case Tokenizer::OPERATOR_INDIRECT: return EXP_TYPE_INDIRECT;

	case Tokenizer::OPERATOR_CONCAT: return EXP_TYPE_CONCAT;
	case Tokenizer::OPERATOR_LENGTH: return EXP_TYPE_LENGTH;

	case Tokenizer::OPERATOR_LOGIC_AND: return EXP_TYPE_LOGIC_AND;
	case Tokenizer::OPERATOR_LOGIC_OR: return EXP_TYPE_LOGIC_OR;
	case Tokenizer::OPERATOR_LOGIC_PTR_EQ: return EXP_TYPE_LOGIC_PTR_EQ;
	case Tokenizer::OPERATOR_LOGIC_PTR_NE: return EXP_TYPE_LOGIC_PTR_NE;
	case Tokenizer::OPERATOR_LOGIC_EQ: return EXP_TYPE_LOGIC_EQ;
	case Tokenizer::OPERATOR_LOGIC_NE: return EXP_TYPE_LOGIC_NE;
	case Tokenizer::OPERATOR_LOGIC_GE: return EXP_TYPE_LOGIC_GE;
	case Tokenizer::OPERATOR_LOGIC_LE: return EXP_TYPE_LOGIC_LE;
	case Tokenizer::OPERATOR_LOGIC_GREATER: return EXP_TYPE_LOGIC_GREATER;
	case Tokenizer::OPERATOR_LOGIC_LESS: return EXP_TYPE_LOGIC_LESS;
	case Tokenizer::OPERATOR_LOGIC_NOT: return EXP_TYPE_LOGIC_NOT;

		// case Tokenizer::OPERATOR_INC: return EXP_TYPE_INC;
		// case Tokenizer::OPERATOR_DEC: return EXP_TYPE_DEC;

	case Tokenizer::OPERATOR_QUESTION: return EXP_TYPE_QUESTION;
		// case Tokenizer::OPERATOR_COLON: return ;
	case Tokenizer::OPERATOR_IN: return EXP_TYPE_IN;
	case Tokenizer::OPERATOR_ISPROTOTYPEOF: return EXP_TYPE_ISPROTOTYPEOF;
	case Tokenizer::OPERATOR_IS: return EXP_TYPE_IS;

	case Tokenizer::OPERATOR_BIT_AND: return EXP_TYPE_BIT_AND;
	case Tokenizer::OPERATOR_BIT_OR: return EXP_TYPE_BIT_OR;
	case Tokenizer::OPERATOR_BIT_XOR: return EXP_TYPE_BIT_XOR;
	case Tokenizer::OPERATOR_BIT_NOT: return EXP_TYPE_BIT_NOT;
	case Tokenizer::OPERATOR_ADD: return EXP_TYPE_ADD;
	case Tokenizer::OPERATOR_SUB: return EXP_TYPE_SUB;
	case Tokenizer::OPERATOR_MUL: return EXP_TYPE_MUL;
	case Tokenizer::OPERATOR_DIV: return EXP_TYPE_DIV;
	case Tokenizer::OPERATOR_MOD: return EXP_TYPE_MOD;
	case Tokenizer::OPERATOR_LSHIFT: return EXP_TYPE_LSHIFT;
	case Tokenizer::OPERATOR_RSHIFT: return EXP_TYPE_RSHIFT;
	case Tokenizer::OPERATOR_POW: return EXP_TYPE_POW;

	case Tokenizer::OPERATOR_BIT_AND_ASSIGN: return EXP_TYPE_BIT_AND_ASSIGN;
	case Tokenizer::OPERATOR_BIT_OR_ASSIGN: return EXP_TYPE_BIT_OR_ASSIGN;
	case Tokenizer::OPERATOR_BIT_XOR_ASSIGN: return EXP_TYPE_BIT_XOR_ASSIGN;
	case Tokenizer::OPERATOR_BIT_NOT_ASSIGN: return EXP_TYPE_BIT_NOT_ASSIGN;
	case Tokenizer::OPERATOR_ADD_ASSIGN: return EXP_TYPE_ADD_ASSIGN;
	case Tokenizer::OPERATOR_SUB_ASSIGN: return EXP_TYPE_SUB_ASSIGN;
	case Tokenizer::OPERATOR_MUL_ASSIGN: return EXP_TYPE_MUL_ASSIGN;
	case Tokenizer::OPERATOR_DIV_ASSIGN: return EXP_TYPE_DIV_ASSIGN;
	case Tokenizer::OPERATOR_MOD_ASSIGN: return EXP_TYPE_MOD_ASSIGN;
	case Tokenizer::OPERATOR_LSHIFT_ASSIGN: return EXP_TYPE_LSHIFT_ASSIGN;
	case Tokenizer::OPERATOR_RSHIFT_ASSIGN: return EXP_TYPE_RSHIFT_ASSIGN;
	case Tokenizer::OPERATOR_POW_ASSIGN: return EXP_TYPE_POW_ASSIGN;

	case Tokenizer::OPERATOR_ASSIGN: return EXP_TYPE_ASSIGN;
	}
	return EXP_TYPE_UNKNOWN;
}

OS::Core::Compiler::OpcodeLevel OS::Core::Compiler::getOpcodeLevel(ExpressionType exp_type)
{
	switch(exp_type){
	case EXP_TYPE_ASSIGN:	// =
	case EXP_TYPE_BIT_AND_ASSIGN: // &=
	case EXP_TYPE_BIT_OR_ASSIGN:  // |=
	case EXP_TYPE_BIT_XOR_ASSIGN: // ^=
	case EXP_TYPE_BIT_NOT_ASSIGN: // ~=
	case EXP_TYPE_ADD_ASSIGN: // +=
	case EXP_TYPE_SUB_ASSIGN: // -=
	case EXP_TYPE_MUL_ASSIGN: // *=
	case EXP_TYPE_DIV_ASSIGN: // /=
	case EXP_TYPE_MOD_ASSIGN: // %=
	case EXP_TYPE_LSHIFT_ASSIGN: // <<=
	case EXP_TYPE_RSHIFT_ASSIGN: // >>=
	case EXP_TYPE_POW_ASSIGN: // **=
		return OP_LEVEL_1;

	case EXP_TYPE_PARAMS:	// ,
		return OP_LEVEL_1_1;

	case EXP_TYPE_QUESTION:    // ? :
		return OP_LEVEL_2;

	case EXP_TYPE_LOGIC_OR:  // ||
		return OP_LEVEL_3;

	case EXP_TYPE_LOGIC_AND: // &&
		return OP_LEVEL_4;

	case EXP_TYPE_CONCAT: // ..
		return OP_LEVEL_5;

	case EXP_TYPE_LOGIC_PTR_EQ:  // ===
	case EXP_TYPE_LOGIC_PTR_NE:  // !==
	case EXP_TYPE_LOGIC_EQ:  // ==
	case EXP_TYPE_LOGIC_NE:  // !=
		return OP_LEVEL_6;

	case EXP_TYPE_LOGIC_GE:  // >=
	case EXP_TYPE_LOGIC_LE:  // <=
	case EXP_TYPE_LOGIC_GREATER: // >
	case EXP_TYPE_LOGIC_LESS:    // <
		return OP_LEVEL_7;

	case EXP_TYPE_BIT_OR:  // |
		return OP_LEVEL_8;

	case EXP_TYPE_BIT_AND: // &
	case EXP_TYPE_BIT_XOR: // ^
		return OP_LEVEL_9;

	case EXP_TYPE_LSHIFT: // <<
	case EXP_TYPE_RSHIFT: // >>
		return OP_LEVEL_10;

	case EXP_TYPE_ADD: // +
	case EXP_TYPE_SUB: // -
		return OP_LEVEL_11;

	case EXP_TYPE_MUL: // *
	case EXP_TYPE_DIV: // /
	case EXP_TYPE_MOD: // %
		return OP_LEVEL_12;

	case EXP_TYPE_POW: // **
	case EXP_TYPE_IN:
	case EXP_TYPE_ISPROTOTYPEOF:
	case EXP_TYPE_IS:
		return OP_LEVEL_13;

	case EXP_TYPE_PRE_INC:     // ++
	case EXP_TYPE_PRE_DEC:     // --
	case EXP_TYPE_POST_INC:    // ++
	case EXP_TYPE_POST_DEC:    // --
		return OP_LEVEL_14;
	/*
	case EXP_TYPE_TYPE_OF:
	case EXP_TYPE_VALUE_OF:
	case EXP_TYPE_NUMBER_OF:
	case EXP_TYPE_STRING_OF:
	case EXP_TYPE_ARRAY_OF:
	case EXP_TYPE_OBJECT_OF:
	case EXP_TYPE_USERDATA_OF:
	case EXP_TYPE_FUNCTION_OF:
	*/
	case EXP_TYPE_LOGIC_BOOL:	// !!
	case EXP_TYPE_LOGIC_NOT:    // !
	case EXP_TYPE_PLUS:			// +
	case EXP_TYPE_NEG:			// -
	case EXP_TYPE_LENGTH:		// #
	case EXP_TYPE_BIT_NOT:		// ~
		return OP_LEVEL_15;

	case EXP_TYPE_INDIRECT:
		return OP_LEVEL_16;
	}
	return OP_LEVEL_0;
}

OS::Core::Tokenizer::TokenData * OS::Core::Compiler::getPrevToken()
{
	int i = next_token_index-2;
	return i >= 0 ? tokenizer->getToken(i) : NULL;
}

OS::Core::Tokenizer::TokenData * OS::Core::Compiler::readToken()
{
	if(isError()){
		return NULL;
	}
	if(next_token_index < tokenizer->getNumTokens()){
		return recent_token = tokenizer->getToken(next_token_index++);
	}
	return recent_token = NULL;
}

OS::Core::Tokenizer::TokenData * OS::Core::Compiler::setNextTokenIndex(int i)
{
	OS_ASSERT(tokenizer && i >= 0 && i <= tokenizer->getNumTokens());
	next_token_index = i;
	return recent_token = next_token_index > 0 ? tokenizer->getToken(next_token_index-1) : NULL;
}

OS::Core::Tokenizer::TokenData * OS::Core::Compiler::setNextToken(TokenData * token)
{
	OS_ASSERT(tokenizer);
	int i, count = tokenizer->getNumTokens();
	for(i = next_token_index - 1; i >= 0; i--){
		if(tokenizer->getToken(i) == token)
			break;
	}
	if(i < 0){
		for(i = next_token_index; i < count; i++){
			if(tokenizer->getToken(i) == token)
				break;
		}
	}
	if(i >= 0 && i < count){
		next_token_index = i;
		return recent_token = next_token_index > 0 ? tokenizer->getToken(next_token_index-1) : NULL;
	}
	OS_ASSERT(false);
	return NULL;
}

OS::Core::Tokenizer::TokenData * OS::Core::Compiler::putNextTokenType(TokenType token_type)
{
	if(token_type == Tokenizer::CODE_SEPARATOR && recent_token && recent_token->type == token_type){
		return ungetToken();
	}
	TokenData * token = recent_token;
	if(readToken()){
		if(recent_token->type == token_type){
			return ungetToken();
		}
		ungetToken();
		token = recent_token;
	}
	if(!token){
		if(next_token_index > 0){
			token = tokenizer->getToken(next_token_index-1);
		}
	}
	if(token){
		token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(token->text_data, String(allocator), token_type, token->line, token->pos);
	}else{
		token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), String(allocator), token_type, 0, 0);
	}
	tokenizer->insertToken(next_token_index, token OS_DBG_FILEPOS);
	return token;
}

OS::Core::Tokenizer::TokenData * OS::Core::Compiler::ungetToken()
{
	return setNextTokenIndex(next_token_index-1);
}

bool OS::Core::Compiler::isNextTokens(TokenType * list, int count)
{
	bool ok = true;
	int save_next_token_index = next_token_index;
	for(int i = 0; i < count; i++){
		if(!readToken() || !recent_token->isTypeOf(list[i])){
			ok = false;
			break;
		}
	}
	setNextTokenIndex(save_next_token_index);
	return ok;
}

bool OS::Core::Compiler::isNextToken(TokenType t0)
{
	return isNextTokens(&t0, 1);
}

bool OS::Core::Compiler::isNextTokens(TokenType t0, TokenType t1)
{
	TokenType list[] = {t0, t1};
	return isNextTokens(list, sizeof(list)/sizeof(list[0]));
}

bool OS::Core::Compiler::isNextTokens(TokenType t0, TokenType t1, TokenType t2)
{
	TokenType list[] = {t0, t1, t2};
	return isNextTokens(list, sizeof(list)/sizeof(list[0]));
}

bool OS::Core::Compiler::isNextTokens(TokenType t0, TokenType t1, TokenType t2, TokenType t3)
{
	TokenType list[] = {t0, t1, t2, t3};
	return isNextTokens(list, sizeof(list)/sizeof(list[0]));
}

void OS::Core::Compiler::deleteNops(ExpressionList& list)
{
	for(int i = 0; i < list.count; i++){
		Expression * exp = list[i];
		switch(exp->type){
		case EXP_TYPE_NOP:
			allocator->deleteObj(exp);
			list.removeIndex(i--);
			break;
		}
	}
}

OS::Core::Tokenizer::TokenData * OS::Core::Compiler::expectToken(TokenType type)
{
	if(isError()){
		return NULL;
	}
	if(!readToken() || recent_token->type != type){
		setError(type, recent_token);
		return NULL;
	}
	return recent_token;
}

OS::Core::Tokenizer::TokenData * OS::Core::Compiler::expectToken()
{
	if(isError()){
		return NULL;
	}
	if(!readToken()){
		setError(ERROR_EXPECT_TOKEN, recent_token);
		return NULL;
	}
	return recent_token;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectExpressionValues(Expression * exp, int ret_values, bool auto_no_values)
{
	if(exp->ret_values == ret_values || ret_values < 0){
		return exp;
	}
	switch(exp->type){
	case EXP_TYPE_CALL:
	case EXP_TYPE_CALL_AUTO_PARAM:
	case EXP_TYPE_CALL_DIM:
		// case EXP_TYPE_GET_DIM:
	case EXP_TYPE_CALL_METHOD:
	case EXP_TYPE_GET_PROPERTY:
	case EXP_TYPE_GET_THIS_PROPERTY_BY_STRING:
	case EXP_TYPE_GET_PROPERTY_BY_LOCALS:
	case EXP_TYPE_GET_PROPERTY_BY_LOCAL_AND_NUMBER:
	case EXP_TYPE_GET_PROPERTY_AUTO_CREATE:
		// case EXP_TYPE_GET_PROPERTY_DIM:
	case EXP_TYPE_INDIRECT:
		// case EXP_TYPE_GET_ENV_VAR_DIM:
	case EXP_TYPE_TAIL_CALL: // ret values are not used for tail call
	case EXP_TYPE_TAIL_CALL_METHOD: // ret values are not used for tail call
		exp->ret_values = ret_values;
		return exp;

	case EXP_TYPE_CODE_LIST:
		if(exp->list.count > 0){
			Expression * last_exp = exp->list[exp->list.count-1];
			switch(last_exp->type){
			case EXP_TYPE_CALL:
			case EXP_TYPE_CALL_AUTO_PARAM:
			case EXP_TYPE_CALL_DIM:
				// case EXP_TYPE_GET_DIM:
			case EXP_TYPE_CALL_METHOD:
			case EXP_TYPE_GET_PROPERTY:
			case EXP_TYPE_GET_THIS_PROPERTY_BY_STRING:
			case EXP_TYPE_GET_PROPERTY_BY_LOCALS:
			case EXP_TYPE_GET_PROPERTY_BY_LOCAL_AND_NUMBER:
			case EXP_TYPE_GET_PROPERTY_AUTO_CREATE:
				// case EXP_TYPE_GET_PROPERTY_DIM:
			case EXP_TYPE_INDIRECT:
				// case EXP_TYPE_GET_ENV_VAR_DIM:
			case EXP_TYPE_TAIL_CALL: // ret values are not used for tail call
			case EXP_TYPE_TAIL_CALL_METHOD: // ret values are not used for tail call
				last_exp->ret_values = ret_values;
				exp->ret_values = ret_values;
				return exp;

			case EXP_TYPE_RETURN:
				last_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CODE_LIST, last_exp->token, last_exp OS_DBG_FILEPOS);
				exp->list[exp->list.count-1] = last_exp;
				last_exp->ret_values = ret_values;
				exp->ret_values = ret_values;
				return exp;
			}
		}
		break;

	case EXP_TYPE_RETURN:
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CODE_LIST, exp->token, exp OS_DBG_FILEPOS);
		exp->ret_values = ret_values;		
		return exp;

	case EXP_TYPE_PARAMS:
		if(exp->ret_values > ret_values){
			for(int i = exp->list.count-1; exp->ret_values > ret_values && i >= 0; i--){
				Expression * param_exp = exp->list[i];
				if(param_exp->type == EXP_TYPE_PARAMS){
					break;
				}
				OS_ASSERT(param_exp->type != EXP_TYPE_PARAMS);
				OS_ASSERT(param_exp->type != EXP_TYPE_RETURN);
				OS_ASSERT(param_exp->type != EXP_TYPE_CODE_LIST);
				if(param_exp->isConstValue()){
					exp->list.removeIndex(i);
					exp->ret_values--;
					allocator->deleteObj(param_exp);
					continue;
				}
				switch(param_exp->type){
				case EXP_TYPE_CALL:
				case EXP_TYPE_CALL_AUTO_PARAM:
				case EXP_TYPE_CALL_DIM:
					// case EXP_TYPE_GET_DIM:
				case EXP_TYPE_CALL_METHOD:
				case EXP_TYPE_GET_PROPERTY:
				case EXP_TYPE_GET_THIS_PROPERTY_BY_STRING:
				case EXP_TYPE_GET_PROPERTY_BY_LOCALS:
				case EXP_TYPE_GET_PROPERTY_BY_LOCAL_AND_NUMBER:
				case EXP_TYPE_GET_PROPERTY_AUTO_CREATE:
					// case EXP_TYPE_GET_PROPERTY_DIM:
					// case EXP_TYPE_GET_ENV_VAR_DIM:
				case EXP_TYPE_INDIRECT:
					if(exp->ret_values <= param_exp->ret_values){
						param_exp->ret_values -= exp->ret_values;
						exp->ret_values = 0;
					}else{
						exp->ret_values -= param_exp->ret_values;
						param_exp->ret_values = 0;
					}
					continue;
				}
				break;
			}
		}
		break;

	case EXP_TYPE_PRE_INC:
	case EXP_TYPE_PRE_DEC:
	case EXP_TYPE_POST_INC:
	case EXP_TYPE_POST_DEC:
		OS_ASSERT(exp->ret_values == 1);
		if(!ret_values){
			exp->ret_values = 0;
			return exp;
		}
		break;
	}
	while(exp->ret_values > ret_values){
		int new_ret_values = exp->ret_values-1;
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_POP_VALUE, exp->token, exp OS_DBG_FILEPOS);
		exp->ret_values = new_ret_values;
	}
	if(exp->ret_values < ret_values){
		if(exp->type != EXP_TYPE_PARAMS){
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, exp->token, exp OS_DBG_FILEPOS);
			exp->ret_values = exp->list[0]->ret_values;
		}
		while(exp->ret_values < ret_values){
			if(auto_no_values && exp->ret_values+1 == ret_values){
				break;
			}
			Expression * null_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_NULL, exp->token);
			null_exp->ret_values = 1;
			exp->list.add(null_exp OS_DBG_FILEPOS);
			exp->ret_values++;
		}
	}
	return exp;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::newSingleValueExpression(Expression * exp)
{
	exp = expectExpressionValues(exp, 1);
	switch(exp->type){
	case EXP_TYPE_CALL:
	case EXP_TYPE_CALL_AUTO_PARAM:
	case EXP_TYPE_CALL_DIM:
		// case EXP_TYPE_GET_DIM:
	case EXP_TYPE_CALL_METHOD:
	case EXP_TYPE_GET_PROPERTY:
	case EXP_TYPE_GET_THIS_PROPERTY_BY_STRING:
	case EXP_TYPE_GET_PROPERTY_BY_LOCALS:
	case EXP_TYPE_GET_PROPERTY_BY_LOCAL_AND_NUMBER:
	case EXP_TYPE_GET_PROPERTY_AUTO_CREATE:
		// case EXP_TYPE_GET_PROPERTY_DIM:
		// case EXP_TYPE_GET_ENV_VAR_DIM:
	case EXP_TYPE_INDIRECT:
		{
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_VALUE, exp->token, exp OS_DBG_FILEPOS);
			exp->ret_values = 1;
			break;
		}
	}
	return exp;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::newExpressionFromList(ExpressionList& list, int ret_values, bool auto_no_values)
{
	Expression * exp;
	if(list.count == 1){
		exp = list[0];
		list.removeIndex(0);
	}else if(list.count == 0){
		TokenData * cur_token = ungetToken();
		readToken();
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CODE_LIST, cur_token);
	}else{
		int i;
		for(i = 0; i < list.count-1; i++){
			OS_ASSERT(list[i]->type != EXP_TYPE_CODE_LIST);
			list[i] = expectExpressionValues(list[i], 0);
		}
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CODE_LIST, list[0]->token);
		exp->list.swap(list);
		exp->ret_values = exp->list[exp->list.count-1]->ret_values;
	}
	return expectExpressionValues(exp, ret_values, auto_no_values);
}

OS::Core::Compiler::Expression * OS::Core::Compiler::postCompilePass2(Scope * scope, Expression * exp)
{
	switch(exp->type){
	case EXP_TYPE_FUNCTION:
		{
			Scope * new_scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(new_scope && (new_scope->parent == scope || (!new_scope->parent && new_scope->type == EXP_TYPE_FUNCTION)));
			if(new_scope != scope){
				new_scope->func_index = scope->function->num_local_funcs++;
				new_scope->func_depth = scope->function->func_depth + 1;
			}
			scope = new_scope;
			OS_ASSERT(prog_functions.indexOf(scope) < 0);
			scope->prog_func_index = prog_functions.count;
			allocator->vectorAddItem(prog_functions, scope OS_DBG_FILEPOS);
			break;
		}

	case EXP_TYPE_SCOPE:
	case EXP_TYPE_LOOP_SCOPE:
		{
			Scope * new_scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(new_scope && (new_scope->parent == scope || (!new_scope->parent && new_scope->type == EXP_TYPE_FUNCTION)));
			scope = new_scope;
			break;
		}

	case EXP_TYPE_DEBUG_LOCALS:
		if(exp->list.count == 0){
			Expression * obj_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_OBJECT, exp->token);

			Vector<String> vars;

			// skip globals & env vars
			allocator->vectorAddItem(vars, allocator->core->strings->syntax_this OS_DBG_FILEPOS);
			allocator->vectorAddItem(vars, allocator->core->strings->var_env OS_DBG_FILEPOS);
#ifdef OS_GLOBAL_VAR_ENABLED
			allocator->vectorAddItem(vars, allocator->core->strings->var_globals OS_DBG_FILEPOS);
#endif

			Scope * start_scope = scope;
			for(; scope; scope = scope->parent){
				for(int i = scope->locals.count-1; i >= 0; i--){
					const Scope::LocalVar& local_var = scope->locals[i];
					if(local_var.name.toChar()[0] == OS_TEXT('#')){
						continue;
					}
					bool found = false;
					for(int j = 0; j < vars.count; j++){
						if(vars[j] == local_var.name){
							found = true;
							break;
						}
					}
					if(found){
						continue;
					}
					allocator->vectorAddItem(vars, local_var.name OS_DBG_FILEPOS);

					TokenData * name_token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), local_var.name, 
						Tokenizer::NAME, exp->token->line, exp->token->pos);

					Expression * var_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_LOCAL_VAR, name_token);
					OS_ASSERT(scope->function);
					var_exp->active_locals = scope->function->num_locals;
					var_exp->ret_values = 1;
					found = findLocalVar(var_exp->local_var, start_scope, local_var.name, start_scope->function->num_locals, true);
					OS_ASSERT(found); // && var_exp->local_var.index == local_var.index);
					if(start_scope->function->max_up_count < var_exp->local_var.up_count){
						start_scope->function->max_up_count = var_exp->local_var.up_count;
					}

					Expression * obj_item_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_OBJECT_SET_BY_NAME, name_token, var_exp OS_DBG_FILEPOS);
					allocator->vectorInsertAtIndex(obj_exp->list, 0, obj_item_exp OS_DBG_FILEPOS);

					name_token->release();
				}
			}
			allocator->vectorClear(vars);
			obj_exp->ret_values = 1;

			scope = start_scope;
			exp->list.add(obj_exp OS_DBG_FILEPOS);
		}
		break;

	case EXP_TYPE_PARAMS:
		{
			for(int i = exp->list.count-1; i >= 0; i--){
				Expression * sub_exp = postCompilePass2(scope, exp->list[i]);
				if(sub_exp->type == EXP_TYPE_PARAMS){
					// OS_ASSERT(false);
					ExpressionList list(allocator);
					int j;
					for(j = 0; j < i; j++){
						list.add(exp->list[j] OS_DBG_FILEPOS);
					}
					for(j = 0; j < sub_exp->list.count; j++){
						list.add(sub_exp->list[j] OS_DBG_FILEPOS);
					}
					for(j = i+1; j < exp->list.count; j++){
						list.add(exp->list[j] OS_DBG_FILEPOS);
					}
					exp->ret_values += sub_exp->ret_values;
					list.swap(exp->list);
					allocator->vectorClear(list);
					allocator->vectorClear(sub_exp->list);
					allocator->deleteObj(sub_exp);
				}else{
					exp->list[i] = sub_exp;
				}
			}
			return exp;
		}

	case EXP_TYPE_POST_INC:
	case EXP_TYPE_POST_DEC:
		OS_ASSERT(exp->list.count == 1);
		if(exp->ret_values > 0){
			OS_ASSERT(exp->ret_values == 1);
			exp->list[0] = postCompilePass2(scope, exp->list[0]);

			Expression * var_exp = exp->list[0];
			OS_ASSERT(var_exp->type == EXP_TYPE_GET_LOCAL_VAR);

			String temp_var_name = String(allocator, OS_TEXT("#temp")); // + String(allocator, (OS_INT)scope->function->num_locals+1);
			TokenData * temp_var_token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), temp_var_name, Tokenizer::NAME, exp->token->line, exp->token->pos);

			TokenData * num_token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), String(allocator, OS_TEXT("1")), Tokenizer::NUMBER, exp->token->line, exp->token->pos);
			num_token->setFloat(1);

			Expression * cur_var_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_LOCAL_VAR, var_exp->token);
			cur_var_exp->ret_values = 1;
			cur_var_exp->local_var = var_exp->local_var;

			Expression * result_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CODE_LIST, exp->token);
			Expression * copy_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_SET_LOCAL_VAR, temp_var_token, cur_var_exp OS_DBG_FILEPOS);
			OS_ASSERT(!findLocalVar(copy_exp->local_var, scope, temp_var_name, scope->function->num_locals, false));
			scope->addLocalVar(temp_var_name, copy_exp->local_var);
			result_exp->list.add(copy_exp OS_DBG_FILEPOS);

			cur_var_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_LOCAL_VAR, var_exp->token);
			cur_var_exp->ret_values = 1;
			cur_var_exp->local_var = var_exp->local_var;

			Expression * num_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_NUMBER, num_token);
			num_exp->ret_values = 1;

			Expression * op_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(exp->type == EXP_TYPE_POST_INC ? EXP_TYPE_ADD : EXP_TYPE_SUB, exp->token, cur_var_exp, num_exp OS_DBG_FILEPOS);
			op_exp->ret_values = 1;

			Expression * set_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_SET_LOCAL_VAR, var_exp->token, op_exp OS_DBG_FILEPOS);
			set_exp->local_var = var_exp->local_var;

			result_exp->list.add(set_exp OS_DBG_FILEPOS);

			Expression * get_temp_var_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_LOCAL_VAR, temp_var_token);
			get_temp_var_exp->ret_values = 1;
			get_temp_var_exp->local_var = copy_exp->local_var;

			result_exp->list.add(get_temp_var_exp OS_DBG_FILEPOS);
			result_exp->ret_values = 1;

			temp_var_token->release();
			num_token->release();

			allocator->deleteObj(exp);
			return postCompilePass2(scope, result_exp);
		}
		exp->type = exp->type == EXP_TYPE_POST_INC ? EXP_TYPE_PRE_INC : EXP_TYPE_PRE_DEC;
		// no break

	case EXP_TYPE_PRE_INC:
	case EXP_TYPE_PRE_DEC:
		{
			OS_ASSERT(exp->list.count == 1);
			exp->list[0] = postCompilePass2(scope, exp->list[0]);

			Expression * var_exp = exp->list[0];
			OS_ASSERT(var_exp->type == EXP_TYPE_GET_LOCAL_VAR);

			TokenData * num_token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), String(allocator, OS_TEXT("1")), Tokenizer::NUMBER, exp->token->line, exp->token->pos);
			num_token->setFloat(1);

			Expression * cur_var_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_LOCAL_VAR, var_exp->token);
			cur_var_exp->ret_values = 1;
			cur_var_exp->local_var = var_exp->local_var;

			Expression * num_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_NUMBER, num_token);
			num_exp->ret_values = 1;

			Expression * op_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(exp->type == EXP_TYPE_PRE_INC ? EXP_TYPE_ADD : EXP_TYPE_SUB, exp->token, cur_var_exp, num_exp OS_DBG_FILEPOS);
			op_exp->ret_values = 1;

			Expression * set_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_SET_LOCAL_VAR, var_exp->token, op_exp OS_DBG_FILEPOS);
			set_exp->local_var = var_exp->local_var;

			Expression * result_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CODE_LIST, exp->token);
			result_exp->list.add(set_exp OS_DBG_FILEPOS);

			if(exp->ret_values > 0){
				OS_ASSERT(exp->ret_values == 1);

				cur_var_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_LOCAL_VAR, var_exp->token);
				cur_var_exp->ret_values = 1;
				cur_var_exp->local_var = var_exp->local_var;

				result_exp->list.add(cur_var_exp OS_DBG_FILEPOS);
				result_exp->ret_values = 1;
			}
			allocator->deleteObj(exp);
			num_token->release();
			return postCompilePass2(scope, result_exp);
		}

	case EXP_TYPE_NAME:
		if(findLocalVar(exp->local_var, scope, exp->token->str, exp->active_locals, true)){
			exp->type = EXP_TYPE_GET_LOCAL_VAR;
			if(scope->function->max_up_count < exp->local_var.up_count){
				scope->function->max_up_count = exp->local_var.up_count;
			}
		}else{
			exp->type = EXP_TYPE_GET_ENV_VAR;
		}
		break;

	/*
	case EXP_TYPE_CONST_NUMBER:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = cacheNumber((OS_NUMBER)exp->token->getFloat());
		break;

	case EXP_TYPE_CONST_STRING:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = cacheString(exp->token->str);
		break;

	case EXP_TYPE_CONST_NULL:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = CONST_NULL;
		break;

	case EXP_TYPE_CONST_FALSE:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = CONST_FALSE;
		break;

	case EXP_TYPE_CONST_TRUE:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = CONST_TRUE;
		break;
	*/

	case EXP_TYPE_RETURN:
		if(exp->list.count == 1){
			Expression * sub_exp = exp->list[0] = postCompilePass2(scope, exp->list[0]);
			switch(sub_exp->type){
			case EXP_TYPE_CALL:
			case EXP_TYPE_CALL_AUTO_PARAM:
				sub_exp->type = EXP_TYPE_TAIL_CALL;
				allocator->vectorClear(exp->list);
				allocator->deleteObj(exp);
				return sub_exp;

			case EXP_TYPE_CALL_METHOD:
				sub_exp->type = EXP_TYPE_TAIL_CALL_METHOD;
				allocator->vectorClear(exp->list);
				allocator->deleteObj(exp);
				return sub_exp;
			}
			return exp;
		}
		break;

	case EXP_TYPE_CALL:
	case EXP_TYPE_CALL_AUTO_PARAM:
		{
			OS_ASSERT(exp->list.count == 2);
			exp->list[0] = postCompilePass2(scope, exp->list[0]);
			exp->list[1] = postCompilePass2(scope, exp->list[1]);
			Expression * left_exp = exp->list[0];
			Expression * right_exp = exp->list[1];
			if(left_exp->type == EXP_TYPE_GET_PROPERTY){
				OS_ASSERT(left_exp->list.count == 2);
				OS_ASSERT(right_exp->type == EXP_TYPE_PARAMS);
				allocator->vectorInsertAtIndex(right_exp->list, 0, left_exp->list[1] OS_DBG_FILEPOS);
				right_exp->ret_values += left_exp->list[1]->ret_values;
				left_exp->list[1] = right_exp;
				left_exp->type = EXP_TYPE_CALL_METHOD;
				left_exp->ret_values = exp->ret_values;
				allocator->vectorClear(exp->list);
				allocator->deleteObj(exp);
				return left_exp;
			}
			if(left_exp->type == EXP_TYPE_GET_ENV_VAR){
				OS_ASSERT(left_exp->list.count == 0);
				OS_ASSERT(right_exp->type == EXP_TYPE_PARAMS);
				left_exp->type = EXP_TYPE_CONST_STRING;
				allocator->vectorInsertAtIndex(right_exp->list, 0, left_exp OS_DBG_FILEPOS);
				right_exp->ret_values++;

				TokenData * name_token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), 
					allocator->core->strings->var_env, 
					Tokenizer::NAME, left_exp->token->line, left_exp->token->pos);

				left_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_LOCAL_VAR, name_token);
				left_exp->ret_values = 1;
				if(!findLocalVar(left_exp->local_var, scope, name_token->str, scope->function->num_params+VAR_ENV+1, true)){
					OS_ASSERT(false);
				};
				if(scope->function->max_up_count < left_exp->local_var.up_count){
					scope->function->max_up_count = left_exp->local_var.up_count;
				}
				exp->list[0] = left_exp;

				name_token->release();

				exp->type = EXP_TYPE_CALL_METHOD;
				return exp;
			}
			return exp;
		}

	case EXP_TYPE_SET_DIM:
		{
			OS_ASSERT(exp->list.count == 3);
			exp->list[0] = postCompilePass2(scope, exp->list[0]);
			exp->list[1] = postCompilePass2(scope, exp->list[1]);
			exp->list[2] = postCompilePass2(scope, exp->list[2]);
			Expression * params = exp->list[2];
			if(params->list.count == 1){
				exp->list[2] = params->list[0];
				allocator->vectorClear(params->list);
				allocator->deleteObj(params);
				exp->type = EXP_TYPE_SET_PROPERTY;
				for(Expression * get_exp = exp->list[1];;){
					switch(get_exp->type){
					case EXP_TYPE_GET_PROPERTY:
						OS_ASSERT(get_exp->list.count == 2);
						get_exp->type = EXP_TYPE_GET_PROPERTY_AUTO_CREATE;
						get_exp = get_exp->list[0];
						continue;

					case EXP_TYPE_GET_LOCAL_VAR:
						get_exp->type = EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE;
						break;

					case EXP_TYPE_GET_ENV_VAR:
						get_exp->type = EXP_TYPE_GET_ENV_VAR_AUTO_CREATE;
						break;
					}
					break;
				}
				return exp;
			}
			if(params->list.count == 0){
				// nop
			}
			break;
		}

	case EXP_TYPE_SET_PROPERTY:
		{
			OS_ASSERT(exp->list.count == 3);
			exp->list[0] = postCompilePass2(scope, exp->list[0]);
			exp->list[1] = postCompilePass2(scope, exp->list[1]);
			exp->list[2] = postCompilePass2(scope, exp->list[2]);
			for(Expression * get_exp = exp->list[1];;){
				switch(get_exp->type){
				case EXP_TYPE_GET_PROPERTY:
					OS_ASSERT(get_exp->list.count == 2);
					get_exp->type = EXP_TYPE_GET_PROPERTY_AUTO_CREATE;
					get_exp = get_exp->list[0];
					continue;

				case EXP_TYPE_GET_LOCAL_VAR:
					get_exp->type = EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE;
					break;

				case EXP_TYPE_GET_ENV_VAR:
					get_exp->type = EXP_TYPE_GET_ENV_VAR_AUTO_CREATE;
					break;
				}
				break;
			}
			return exp;
		}

	case EXP_TYPE_CALL_DIM:
		{
			OS_ASSERT(exp->list.count == 2);
			exp->list[0] = postCompilePass2(scope, exp->list[0]);
			exp->list[1] = postCompilePass2(scope, exp->list[1]);
			Expression * name_exp = exp->list[0];
			Expression * params = exp->list[1];
			OS_ASSERT(params->type == EXP_TYPE_PARAMS);
			if(params->list.count == 1){
				exp->list[1] = params->list[0];
				allocator->vectorClear(params->list);
				allocator->deleteObj(params);
				exp->type = EXP_TYPE_GET_PROPERTY;
			}else{
				// exp->type = EXP_TYPE_GET_DIM;
				String method_name = !params->list.count ? allocator->core->strings->__getempty : allocator->core->strings->__getdim;
				TokenData * token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), method_name, Tokenizer::NAME, name_exp->token->line, name_exp->token->pos);
				Expression * exp_method_name = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_STRING, token);
				exp_method_name->ret_values = 1;
				token->release();

				allocator->vectorInsertAtIndex(params->list, 0, exp_method_name OS_DBG_FILEPOS);
				params->ret_values++;

				exp->type = EXP_TYPE_CALL_METHOD;
			}
			return exp;
		}

	case EXP_TYPE_INDIRECT:
		{
			OS_ASSERT(exp->list.count == 2);
			exp->list[0] = expectExpressionValues(exp->list[0], 1);
			exp->list[1] = expectExpressionValues(exp->list[1], 1);
			Expression * left_exp = exp->list[0];
			Expression * right_exp = exp->list[1];
			ExpressionType exp_type = EXP_TYPE_GET_PROPERTY;
			switch(right_exp->type){
			case EXP_TYPE_NAME:
				right_exp->type = EXP_TYPE_CONST_STRING;
				break;

			case EXP_TYPE_CALL:
			case EXP_TYPE_CALL_AUTO_PARAM:
				right_exp->type = EXP_TYPE_PARAMS;
				exp_type = EXP_TYPE_CALL_METHOD;
				OS_ASSERT(right_exp->list.count == 2);
				if(right_exp->list[0]->type == EXP_TYPE_NAME){
					OS_ASSERT(right_exp->list[0]->ret_values == 1);
					right_exp->list[0]->type = EXP_TYPE_CONST_STRING;
				}
				break;
			}
			exp->type = exp_type;
			break;
		}
	}
	for(int i = 0; i < exp->list.count; i++){
		exp->list[i] = postCompilePass2(scope, exp->list[i]);
	}
	return exp;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::postCompileExpression(Scope * scope, Expression * exp)
{
	exp = postCompilePass2(scope, exp);
#if 0
	return exp;
#elif 1
	OS_ASSERT(scope->type == EXP_TYPE_FUNCTION);
	exp = postCompilePass3(scope, exp);
	exp = postCompileNewVM(scope, exp);
	OS_ASSERT(scope->function->stack_cur_size == scope->function->num_locals);
	return exp;
#else
	OS_ASSERT(scope->type == EXP_TYPE_FUNCTION);
	// prog_stack_size = 0;
	return postCompilePass3(scope, exp);
#endif
}

OS::Core::Compiler::Expression * OS::Core::Compiler::postCompilePass3(Scope * scope, Expression * exp)
{
	struct Lib {
		static Expression * processList(Compiler * compiler, Scope * scope, Expression * exp)
		{
			for(int i = 0; i < exp->list.count; i++){
				exp->list[i] = compiler->postCompilePass3(scope, exp->list[i]);
			}
			return exp;
		}
	};

	switch(exp->type){
	case EXP_TYPE_FUNCTION:
		{
			Scope * new_scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(new_scope && (new_scope->parent == scope || (!new_scope->parent && new_scope->type == EXP_TYPE_FUNCTION)));
			scope = new_scope;
			break;
		}

	case EXP_TYPE_SCOPE:
	case EXP_TYPE_LOOP_SCOPE:
		{
			Scope * new_scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(new_scope && (new_scope->parent == scope || (!new_scope->parent && new_scope->type == EXP_TYPE_FUNCTION)));
			scope = new_scope;
			break;
		}

	case EXP_TYPE_CALL:
	case EXP_TYPE_CALL_AUTO_PARAM:
	case EXP_TYPE_TAIL_CALL:
		{
			OS_ASSERT(exp->list.count == 2);
			Expression * exp_params = exp->list[1];
			OS_ASSERT(exp_params->type == EXP_TYPE_PARAMS);
			Expression * exp_this = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_NULL, exp_params->token);
			exp_this->ret_values = 1;
			allocator->vectorInsertAtIndex(exp_params->list, 0, exp_this OS_DBG_FILEPOS);
			exp_params->ret_values++;
			return Lib::processList(this, scope, exp);
		}

	case EXP_TYPE_GET_ENV_VAR:
	case EXP_TYPE_GET_ENV_VAR_AUTO_CREATE:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = cacheString(exp->token->str);
		break;

	case EXP_TYPE_SET_ENV_VAR:
		OS_ASSERT(exp->list.count == 1);
		exp->slots.b = cacheString(exp->token->str);
		break;

	case EXP_TYPE_DELETE:
		OS_ASSERT(exp->list.count == 2);
		exp->slots.b = cacheString(allocator->core->strings->__del);
		break;

	case EXP_TYPE_SET_DIM:
		OS_ASSERT(exp->list.count == 3);
		OS_ASSERT(exp->list[2]->type == EXP_TYPE_PARAMS);
		exp->slots.b = cacheString(exp->list[2]->ret_values > 0 ? allocator->core->strings->__setdim : allocator->core->strings->__setempty);
		break;

	case EXP_TYPE_LENGTH:
		OS_ASSERT(exp->list.count == 1);
		exp->slots.b = cacheString(allocator->core->strings->__len);
		break;

	case EXP_TYPE_OBJECT_SET_BY_NAME:
		OS_ASSERT(exp->list.count == 1);
		exp->slots.b = cacheString(exp->token->str);
		break;

	case EXP_TYPE_OBJECT_SET_BY_INDEX:
		OS_ASSERT(exp->list.count == 1);
		exp->slots.b = cacheNumber((OS_NUMBER)exp->token->getFloat());
		break;

	case EXP_TYPE_OBJECT_SET_BY_AUTO_INDEX:
		OS_ASSERT(exp->list.count == 1);
		exp->slots.b = cacheString(allocator->core->strings->func_push);
		break;

	case EXP_TYPE_EXTENDS:
		OS_ASSERT(exp->list.count == 2);
		exp->slots.b = cacheString(allocator->core->strings->func_extends);
		break;

	case EXP_TYPE_IN:
		OS_ASSERT(exp->list.count == 2);
		exp->slots.b = cacheString(allocator->core->strings->func_in);
		break;

	case EXP_TYPE_IS:
		OS_ASSERT(exp->list.count == 2);
		exp->slots.b = cacheString(allocator->core->strings->func_is);
		break;

	case EXP_TYPE_ISPROTOTYPEOF:
		OS_ASSERT(exp->list.count == 2);
		exp->slots.b = cacheString(allocator->core->strings->func_isprototypeof);
		break;

	case EXP_TYPE_CONST_NUMBER:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = cacheNumber((OS_NUMBER)exp->token->getFloat());
		break;

	case EXP_TYPE_CONST_STRING:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = cacheString(exp->token->str);
		break;

	case EXP_TYPE_CONST_NULL:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = CONST_NULL;
		break;

	case EXP_TYPE_CONST_FALSE:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = CONST_FALSE;
		break;

	case EXP_TYPE_CONST_TRUE:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.b = CONST_TRUE;
		break;
	}
	return Lib::processList(this, scope, exp);
}
OS::Core::Compiler::Expression * OS::Core::Compiler::postCompileNewVM(Scope * scope, Expression * exp)
{
	struct Lib {
		static Expression * processList(Compiler * compiler, Scope * scope, Expression * exp)
		{
			for(int i = 0; i < exp->list.count; i++){
				exp->list[i] = compiler->postCompileNewVM(scope, exp->list[i]);
			}
			return exp;
		}
		
		static bool allowOverrideOpcodeResult(Expression * exp)
		{
			switch(exp->type){
			case EXP_TYPE_FUNCTION:
			case EXP_TYPE_MOVE:
			case EXP_TYPE_CONST_NUMBER:
			case EXP_TYPE_CONST_STRING:
			case EXP_TYPE_CONST_NULL:
			case EXP_TYPE_CONST_FALSE:
			case EXP_TYPE_CONST_TRUE:
			case EXP_TYPE_CONCAT:
			case EXP_TYPE_LOGIC_PTR_EQ:
			case EXP_TYPE_LOGIC_PTR_NE:
			case EXP_TYPE_LOGIC_EQ:
			case EXP_TYPE_LOGIC_NE:
			case EXP_TYPE_LOGIC_GE:
			case EXP_TYPE_LOGIC_LE:
			case EXP_TYPE_LOGIC_GREATER:
			case EXP_TYPE_LOGIC_LESS:
			case EXP_TYPE_BIT_AND:
			case EXP_TYPE_BIT_OR:
			case EXP_TYPE_BIT_XOR:
			case EXP_TYPE_ADD: // +
			case EXP_TYPE_SUB: // -
			case EXP_TYPE_MUL: // *
			case EXP_TYPE_DIV: // /
			case EXP_TYPE_MOD: // %
			case EXP_TYPE_LSHIFT: // <<
			case EXP_TYPE_RSHIFT: // >>
			case EXP_TYPE_POW: // **
			case EXP_TYPE_GET_UPVALUE_VAR:
				return true;
			}
			return false;
		}

		static Expression * addXconst(Compiler * compiler, Expression * exp, Expression * xconst)
		{
			if(xconst){
				return new (compiler->malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CODE_LIST, exp->token, xconst, exp OS_DBG_FILEPOS);
			}
			return exp;
		}
	};
	Expression * exp1, * exp2, * exp_xconst;
	int stack_pos, b;
	switch(exp->type){
	default:
		OS_ASSERT(false);
		break;

	case EXP_TYPE_FUNCTION:
		{
			Scope * new_scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(new_scope && (new_scope->parent == scope || (!new_scope->parent && new_scope->type == EXP_TYPE_FUNCTION)));
			scope = new_scope;
			scope->slots.a = scope->parent ? scope->parent->allocTempVar() : 0;
			scope->slots.b = scope->prog_func_index;
			break;
		}

	case EXP_TYPE_SCOPE:
	case EXP_TYPE_LOOP_SCOPE:
		{
			Scope * new_scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(new_scope && (new_scope->parent == scope || (!new_scope->parent && new_scope->type == EXP_TYPE_FUNCTION)));
			scope = new_scope;
			break;
		}

	case EXP_TYPE_CODE_LIST:
	case EXP_TYPE_PARAMS:
	case EXP_TYPE_MOVE:
	case EXP_TYPE_NEW_LOCAL_VAR:
	case EXP_TYPE_BREAK:
	case EXP_TYPE_CONTINUE:
	case EXP_TYPE_NOP:
		break;

	case EXP_TYPE_POP_VALUE:
		exp = Lib::processList(this, scope, exp);
		scope->popTempVar();
		return exp;

	case EXP_TYPE_VALUE:
		break;
		// stack_pos = scope->function->stack_cur_size;
		// exp = Lib::processList(this, scope, exp);
		// return exp;

	case EXP_TYPE_RETURN:
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		exp->slots.a = stack_pos;
		exp->slots.b = scope->function->stack_cur_size - stack_pos;
		exp->slots.c = 0;
		OS_ASSERT(exp->slots.b == exp->ret_values);
		scope->function->stack_cur_size = stack_pos;
		if(exp->slots.b == 1){
			OS_ASSERT(exp->list.count == 1);
			exp1 = exp->list[0];
			if(exp1->type == EXP_TYPE_MOVE){ // && exp1->slots.a >= scope->function->num_locals){ // stack_cur_size is already decremented
				exp->slots.a = exp1->slots.b;
				exp1->type = EXP_TYPE_NOP;
			}
		}
		return exp;

	case EXP_TYPE_CALL:
	case EXP_TYPE_CALL_AUTO_PARAM:
	case EXP_TYPE_TAIL_CALL:
		OS_ASSERT(exp->list.count == 2);
		OS_ASSERT(exp->list[1]->type == EXP_TYPE_PARAMS);
		exp1 = exp->list[0];
		if(exp1->type == EXP_TYPE_SUPER){
			exp1->type = EXP_TYPE_CONST_NULL;
			exp->type = EXP_TYPE_SUPER_CALL;
		}
		// no break

	case EXP_TYPE_CALL_METHOD:
	case EXP_TYPE_TAIL_CALL_METHOD:
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		exp->slots.a = stack_pos;
		exp->slots.b = scope->function->stack_cur_size - stack_pos;
		exp->slots.c = exp->ret_values;
		scope->function->stack_cur_size = stack_pos + exp->ret_values;
		if(scope->function->stack_size < scope->function->stack_cur_size){
			scope->function->stack_size = scope->function->stack_cur_size;
		}
		return exp;

	case EXP_TYPE_IF:
		OS_ASSERT(exp->list.count == 2 || exp->list.count == 3);
		stack_pos = scope->function->stack_cur_size;
		exp->list[0] = exp1 = postCompileNewVM(scope, exp->list[0]);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);
		scope->popTempVar();
		exp->list[1] = exp1 = postCompileNewVM(scope, exp->list[1]);
		OS_ASSERT(stack_pos == scope->function->stack_cur_size);
		if(exp->list.count == 3){
			exp->list[2] = exp1 = postCompileNewVM(scope, exp->list[2]);
			OS_ASSERT(stack_pos == scope->function->stack_cur_size);
		}
		OS_ASSERT(stack_pos == scope->function->stack_cur_size);
		return exp;

	case EXP_TYPE_QUESTION:
		OS_ASSERT(exp->list.count == 3);
		stack_pos = scope->function->stack_cur_size;
		
		exp->list[0] = exp1 = postCompileNewVM(scope, exp->list[0]);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);
		scope->popTempVar();
		
		exp->list[1] = exp1 = postCompileNewVM(scope, exp->list[1]);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);
		scope->popTempVar();
		
		exp->list[2] = exp1 = postCompileNewVM(scope, exp->list[2]);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);
		return exp;

	case EXP_TYPE_DEBUG_LOCALS:
		OS_ASSERT(exp->list.count == 1);
		exp1 = postCompileNewVM(scope, exp->list[0]);
		allocator->vectorClear(exp->list);
		allocator->deleteObj(exp);
		return exp1;

	case EXP_TYPE_ARRAY:
	case EXP_TYPE_OBJECT:
		exp->slots.a = scope->allocTempVar();
		break;

	case EXP_TYPE_OBJECT_SET_BY_NAME:
		OS_ASSERT(exp->list.count == 1);

		stack_pos = scope->allocTempVar();
		b = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES; // const string
		if(b < -MAX_GENERIC_CONST_INDEX){
			exp_xconst = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_XCONST, exp->token);
			exp_xconst->slots.b = b;
			exp_xconst->slots.a = b = stack_pos;
		}else exp_xconst = NULL;

		exp->type = EXP_TYPE_SET_PROPERTY;
		OS_ASSERT(scope->function->stack_cur_size > scope->function->num_locals);
		exp->slots.a = scope->function->stack_cur_size-2;
		exp->slots.b = b;
		exp->list[0] = exp1 = postCompileNewVM(scope, exp->list[0]);
		exp->slots.c = exp1->slots.a;
		scope->popTempVar(2);
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.c = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}
		return Lib::addXconst(this, exp, exp_xconst);

	case EXP_TYPE_OBJECT_SET_BY_INDEX:
		OS_ASSERT(exp->list.count == 1);

		stack_pos = scope->allocTempVar();
		b = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES; // const string
		if(b < -MAX_GENERIC_CONST_INDEX){
			exp_xconst = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_XCONST, exp->token);
			exp_xconst->slots.b = b;
			exp_xconst->slots.a = b = stack_pos;
		}else exp_xconst = NULL;

		exp->type = EXP_TYPE_SET_PROPERTY;
		OS_ASSERT(scope->function->stack_cur_size > scope->function->num_locals);
		exp->slots.a = scope->function->stack_cur_size-2;
		exp->slots.b = b;
		exp->list[0] = exp1 = postCompileNewVM(scope, exp->list[0]);
		exp->slots.c = exp1->slots.a;
		scope->popTempVar(2);
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.c = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}
		return Lib::addXconst(this, exp, exp_xconst);

	case EXP_TYPE_OBJECT_SET_BY_EXP:
		OS_ASSERT(exp->list.count == 2);
		exp->type = EXP_TYPE_SET_PROPERTY;
		OS_ASSERT(scope->function->stack_cur_size > scope->function->num_locals);
		exp->slots.a = scope->function->stack_cur_size-1;
		exp->list[0] = exp1 = postCompileNewVM(scope, exp->list[0]);
		exp->list[1] = exp2 = postCompileNewVM(scope, exp->list[1]);
		exp->slots.b = exp1->slots.a;
		exp->slots.c = exp2->slots.a;
		scope->popTempVar(2);
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.b = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}
		if(exp2->type == EXP_TYPE_MOVE){
			exp->slots.c = exp2->slots.b;
			exp2->type = EXP_TYPE_NOP;
		}
		return exp;

	case EXP_TYPE_OBJECT_SET_BY_AUTO_INDEX:
		stack_pos = scope->function->stack_cur_size;

		exp->type = EXP_TYPE_CALL_METHOD;
		exp->ret_values = 0;

		exp1 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, exp->token);
		exp1->list.swap(exp->list);
		exp1->ret_values = 1;

		exp2 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_MOVE, exp->token);
		exp2->slots.a = scope->allocTempVar();
		exp2->slots.b = scope->function->stack_cur_size-2;
		exp->list.add(exp2 OS_DBG_FILEPOS);
		exp->list.add(exp1 OS_DBG_FILEPOS);

		exp2 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_MOVE, exp->token);
		exp2->slots.a = scope->allocTempVar();
		exp2->slots.b = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES; // const string
		allocator->vectorInsertAtIndex(exp1->list, 0, exp2 OS_DBG_FILEPOS);

		if(exp2->slots.b < -MAX_GENERIC_CONST_INDEX){
			exp2->type = EXP_TYPE_GET_XCONST;
		}

		exp1->list[1] = postCompileNewVM(scope, exp1->list[1]);
		OS_ASSERT(scope->function->stack_cur_size - stack_pos == 3);
		exp->slots.a = stack_pos;
		exp->slots.b = 3;
		exp->slots.c = 0;
		// exp = Lib::processList(this, scope, exp);
		scope->function->stack_cur_size = stack_pos;
		// scope->popTempVar(2);
		return exp;

	case EXP_TYPE_CONST_NUMBER:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.a = scope->allocTempVar();
		exp->slots.b = -1 - exp->slots.b - CONST_STD_VALUES;
		exp->type = exp->slots.b < -MAX_GENERIC_CONST_INDEX ? EXP_TYPE_GET_XCONST : EXP_TYPE_MOVE;
		return exp;

	case EXP_TYPE_CONST_STRING:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.a = scope->allocTempVar();
		exp->slots.b = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES;
		exp->type = exp->slots.b < -MAX_GENERIC_CONST_INDEX ? EXP_TYPE_GET_XCONST : EXP_TYPE_MOVE;
		return exp;

	case EXP_TYPE_CONST_NULL:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.a = scope->allocTempVar();
		exp->slots.b = -1 - CONST_NULL;
		exp->type = EXP_TYPE_MOVE;
		return exp;

	case EXP_TYPE_CONST_FALSE:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.a = scope->allocTempVar();
		exp->slots.b = -1 - CONST_FALSE;
		exp->type = EXP_TYPE_MOVE;
		return exp;

	case EXP_TYPE_CONST_TRUE:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.a = scope->allocTempVar();
		exp->slots.b = -1 - CONST_TRUE;
		exp->type = EXP_TYPE_MOVE;
		return exp;

	case EXP_TYPE_GET_THIS:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.a = scope->allocTempVar();
		exp->slots.b = scope->function->num_params + VAR_THIS;
		exp->type = EXP_TYPE_MOVE;
		return exp;

	case EXP_TYPE_GET_ARGUMENTS:
	case EXP_TYPE_GET_REST_ARGUMENTS:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.a = scope->allocTempVar();
		return exp;

	case EXP_TYPE_LOGIC_NOT: // !
		OS_ASSERT(exp->list.count == 1);
		stack_pos = scope->function->stack_cur_size;
		exp1 = postCompileNewVM(scope, exp->list[0]);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);
		if(exp1->type == EXP_TYPE_LOGIC_NOT){
			OS_ASSERT(exp1->list.count == 1);
			exp->type = EXP_TYPE_LOGIC_BOOL;
			exp->list[0] = exp1->list[0];
			allocator->vectorRemoveAtIndex(exp1->list, 0);
			allocator->deleteObj(exp1);
			return exp;
		}
		if(exp1->type == EXP_TYPE_LOGIC_BOOL){
			OS_ASSERT(exp1->list.count == 1);
			exp->list[0] = exp1->list[0];
			allocator->vectorRemoveAtIndex(exp1->list, 0);
			allocator->deleteObj(exp1);
			return exp;
		}
		exp->list[0] = exp1;
		OS_ASSERT(exp1->ret_values == 1);
		exp->slots.a = stack_pos;
		exp->slots.b = stack_pos;
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.b = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}
		return exp;

	case EXP_TYPE_LOGIC_BOOL: // !
		OS_ASSERT(exp->list.count == 1);
		stack_pos = scope->function->stack_cur_size;
		exp1 = postCompileNewVM(scope, exp->list[0]);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);
		if(exp1->type == EXP_TYPE_LOGIC_NOT || exp1->type == EXP_TYPE_LOGIC_BOOL){
			allocator->vectorRemoveAtIndex(exp->list, 0);
			allocator->deleteObj(exp);
			return exp1;
		}
		exp->list[0] = exp1;
		OS_ASSERT(exp1->ret_values == 1);
		exp->slots.a = stack_pos;
		exp->slots.b = stack_pos;
		return exp;

	case EXP_TYPE_EXTENDS:
	case EXP_TYPE_IN:
	case EXP_TYPE_IS:
	case EXP_TYPE_ISPROTOTYPEOF:
		OS_ASSERT(exp->list.count == 2 && exp->ret_values == 1);
		stack_pos = scope->function->stack_cur_size;

		exp1 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, exp->token);
		exp1->list.swap(exp->list);
		exp1->ret_values = 2;

		exp2 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_MOVE, exp->token);
		exp2->slots.a = scope->allocTempVar();
		exp2->slots.b = scope->function->num_params + VAR_ENV;
		exp2->ret_values = 1;
		exp->list.add(exp2 OS_DBG_FILEPOS);

		exp2 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_MOVE, exp->token);
		exp2->slots.a = scope->allocTempVar();
		exp2->slots.b = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES;
		exp2->ret_values = 1;
		allocator->vectorInsertAtIndex(exp1->list, 0, exp2 OS_DBG_FILEPOS);

		if(exp2->slots.b < -MAX_GENERIC_CONST_INDEX){
			exp2->type = EXP_TYPE_GET_XCONST;
		}

		OS_ASSERT(exp1->list.count == 3);
		exp1->list[1] = postCompileNewVM(scope, exp1->list[1]);
		exp1->list[2] = postCompileNewVM(scope, exp1->list[2]);
		OS_ASSERT(stack_pos+4 == scope->function->stack_cur_size);

		exp->list.add(exp1 OS_DBG_FILEPOS); // params

		exp->type = EXP_TYPE_CALL_METHOD;
		exp->slots.a = stack_pos;
		exp->slots.b = scope->function->stack_cur_size - stack_pos;
		exp->slots.c = exp->ret_values;
		scope->function->stack_cur_size = stack_pos + exp->ret_values;
		return exp;

	case EXP_TYPE_DELETE:
		OS_ASSERT(exp->list.count == 2 && exp->ret_values == 0);
		stack_pos = scope->function->stack_cur_size;
		exp->list[0] = postCompileNewVM(scope, exp->list[0]);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);

		exp1 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, exp->token);
		
		exp2 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_MOVE, exp->token);
		exp2->slots.a = scope->allocTempVar();
		exp2->slots.b = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES;
		exp2->ret_values = 1;
		exp1->list.add(exp2 OS_DBG_FILEPOS);
		
		if(exp2->slots.b < -MAX_GENERIC_CONST_INDEX){
			exp2->type = EXP_TYPE_GET_XCONST;
		}

		exp1->list.add(postCompileNewVM(scope, exp->list[1]) OS_DBG_FILEPOS);
		exp1->ret_values = 2;

		exp->list[1] = exp1;
		
		OS_ASSERT(stack_pos+3 == scope->function->stack_cur_size);

		exp->type = EXP_TYPE_CALL_METHOD;
		exp->slots.a = stack_pos;
		exp->slots.b = scope->function->stack_cur_size - stack_pos;
		exp->slots.c = 0;
		scope->function->stack_cur_size = stack_pos;
		return exp;

	case EXP_TYPE_SET_DIM:
		{
			OS_ASSERT(exp->list.count == 3);
			stack_pos = scope->function->stack_cur_size;
			exp->list[0] = postCompileNewVM(scope, exp->list[0]);
			OS_ASSERT(stack_pos+1 <= scope->function->stack_cur_size);

			stack_pos = scope->function->stack_cur_size;
			exp->list[1] = postCompileNewVM(scope, exp->list[1]);
			OS_ASSERT(stack_pos+1 <= scope->function->stack_cur_size);

			exp1 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CODE_LIST, exp->token);
			exp1->list.swap(exp->list);
			exp->list.add(exp1 OS_DBG_FILEPOS);

			exp2 = exp1->list[2];
			OS_ASSERT(exp2->type == EXP_TYPE_PARAMS);
			allocator->vectorRemoveAtIndex(exp1->list, 2);
			exp->list.add(exp2 OS_DBG_FILEPOS);

			exp1 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_MOVE, exp->token);
			exp1->slots.a = scope->allocTempVar();
			exp1->slots.b = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES;
			exp1->ret_values = 1;
			allocator->vectorInsertAtIndex(exp2->list, 0, exp1 OS_DBG_FILEPOS);

			if(exp2->slots.b < -MAX_GENERIC_CONST_INDEX){
				exp2->type = EXP_TYPE_GET_XCONST;
			}

			exp1 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_MOVE, exp->token);
			exp1->slots.a = scope->allocTempVar();
			exp1->slots.b = scope->function->stack_cur_size - 4;
			exp1->ret_values = 1;
			allocator->vectorInsertAtIndex(exp2->list, 1, exp1 OS_DBG_FILEPOS);

			for(int i = 2; i < exp2->list.count; i++){
				exp2->list[i] = postCompileNewVM(scope, exp2->list[i]);
			}
			exp->type = EXP_TYPE_CALL_METHOD;
			exp->slots.a = stack_pos;
			exp->slots.b = scope->function->stack_cur_size - stack_pos;
			exp->slots.c = 0;
			scope->function->stack_cur_size = stack_pos - 1;
			return exp;
		}

	case EXP_TYPE_LENGTH: // #
		OS_ASSERT(exp->list.count == 1);
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);

		exp1 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, exp->token);
		exp1->ret_values = 1;
		
		exp2 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_MOVE, exp->token);
		exp2->slots.a = scope->allocTempVar();
		exp2->slots.b = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES;
		exp2->ret_values = 1;
		exp1->list.add(exp2 OS_DBG_FILEPOS);
		exp->list.add(exp1 OS_DBG_FILEPOS);

		if(exp2->slots.b < -MAX_GENERIC_CONST_INDEX){
			exp2->type = EXP_TYPE_GET_XCONST;
		}

		exp->type = EXP_TYPE_CALL_METHOD;
		exp->slots.a = stack_pos;
		exp->slots.b = scope->function->stack_cur_size - stack_pos;
		exp->slots.c = exp->ret_values;
		scope->function->stack_cur_size = stack_pos + exp->ret_values;
		return exp;

	case EXP_TYPE_BIT_NOT:		// ~
	case EXP_TYPE_PLUS:			// +
	case EXP_TYPE_NEG:			// -
		OS_ASSERT(exp->list.count == 1);
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);
		exp->slots.a = stack_pos;
		exp->slots.b = stack_pos;
		exp1 = exp->list[0];
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.b = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}		
		return exp;

	case EXP_TYPE_LOGIC_AND:
	case EXP_TYPE_LOGIC_OR:
		OS_ASSERT(exp->list.count == 2);
		stack_pos = scope->function->stack_cur_size;
		
		exp->list[0] = exp1 = postCompileNewVM(scope, exp->list[0]);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);
		scope->popTempVar();

		exp->list[1] = exp2 = postCompileNewVM(scope, exp->list[1]);
		OS_ASSERT(stack_pos+1 == scope->function->stack_cur_size);
		OS_ASSERT(stack_pos == exp1->slots.a && stack_pos == exp2->slots.a);
		
		exp->slots.a = stack_pos;
		return exp;

	case EXP_TYPE_CONCAT:
	case EXP_TYPE_BIT_AND:
	case EXP_TYPE_BIT_OR:
	case EXP_TYPE_BIT_XOR:
	case EXP_TYPE_ADD: // +
	case EXP_TYPE_SUB: // -
	case EXP_TYPE_MUL: // *
	case EXP_TYPE_DIV: // /
	case EXP_TYPE_MOD: // %
	case EXP_TYPE_LSHIFT: // <<
	case EXP_TYPE_RSHIFT: // >>
	case EXP_TYPE_POW: // **
		OS_ASSERT(exp->list.count == 2);
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		exp1 = exp->list[0];
		exp2 = exp->list[1];
		OS_ASSERT(stack_pos+2 == scope->function->stack_cur_size);
		// OS_ASSERT(stack_pos == exp1->slots.a && stack_pos+1 == exp2->slots.a);
		exp->slots.a = stack_pos;
		exp->slots.b = stack_pos;
		exp->slots.c = stack_pos+1;
		scope->popTempVar();
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.b = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}
		if(exp2->type == EXP_TYPE_MOVE){
			exp->slots.c = exp2->slots.b;
			exp2->type = EXP_TYPE_NOP;
		}
		return exp;

	case EXP_TYPE_LOGIC_PTR_EQ:
	case EXP_TYPE_LOGIC_PTR_NE:
	case EXP_TYPE_LOGIC_EQ:
	case EXP_TYPE_LOGIC_NE:
	case EXP_TYPE_LOGIC_GE:
	case EXP_TYPE_LOGIC_LE:
	case EXP_TYPE_LOGIC_GREATER:
	case EXP_TYPE_LOGIC_LESS:
		OS_ASSERT(exp->list.count == 2);
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		OS_ASSERT(stack_pos+2 == scope->function->stack_cur_size);
		exp->slots.a = stack_pos;
		exp->slots.b = 0;
		exp->slots.c = 0;
		scope->popTempVar();
		return exp;

	case EXP_TYPE_GET_LOCAL_VAR:
	case EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE:
		OS_ASSERT(exp->list.count == 0);
		if(exp->local_var.up_count){
			exp->type = EXP_TYPE_GET_UPVALUE_VAR;
			exp->slots.a = scope->allocTempVar();
			exp->slots.b = exp->local_var.index;
			exp->slots.c = exp->local_var.up_count;
		}else{
			exp->type = EXP_TYPE_MOVE;
			exp->slots.a = scope->allocTempVar();
			exp->slots.b = exp->local_var.index;
		}
		return exp;

	case EXP_TYPE_SET_LOCAL_VAR:
		OS_ASSERT(exp->list.count == 1);
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		OS_ASSERT(stack_pos < scope->function->stack_cur_size);
		if(exp->local_var.up_count){
			exp->type = EXP_TYPE_SET_UPVALUE_VAR;
			exp->slots.a = exp->local_var.index;
			exp->slots.b = --scope->function->stack_cur_size;
			exp->slots.c = exp->local_var.up_count;
		}else{
			exp->type = EXP_TYPE_MOVE;
			exp->slots.a = exp->local_var.index;
			exp->slots.b = --scope->function->stack_cur_size;
		}
		exp1 = exp->list[0];
		if(exp1->type == EXP_TYPE_PARAMS){
			OS_ASSERT(exp1->list.count > 0);
			exp1 = exp1->list.lastElement();
		}
		if(exp1->type == EXP_TYPE_MOVE && exp1->slots.a >= scope->function->num_locals){ // stack_cur_size is already decremented
			exp->slots.b = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
			return exp;
		}
		if(exp->type == EXP_TYPE_MOVE 
			&& Lib::allowOverrideOpcodeResult(exp1)
			&& exp1->slots.a >= scope->function->num_locals) // stack_cur_size is already decremented
		{
			exp1->slots.a = exp->slots.a;
			exp->type = EXP_TYPE_NOP;
		}
		return exp;

	case EXP_TYPE_SET_PROPERTY:
		OS_ASSERT(exp->list.count == 3);
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		OS_ASSERT(stack_pos+3 <= scope->function->stack_cur_size);
		stack_pos = scope->function->stack_cur_size - 3;
		exp->slots.a = stack_pos + 1;
		exp->slots.b = stack_pos + 2;
		exp->slots.c = stack_pos;
		scope->function->stack_cur_size = stack_pos;
		exp1 = exp->list[0];
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.c = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}
		exp1 = exp->list[1];
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.a = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}
		exp1 = exp->list[2];
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.b = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}
		return exp;

	case EXP_TYPE_GET_ENV_VAR:
	case EXP_TYPE_GET_ENV_VAR_AUTO_CREATE:
		OS_ASSERT(exp->list.count == 0);
		exp->slots.a = scope->allocTempVar();
		exp->slots.c = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES;
		exp->slots.b = scope->function->num_params + VAR_ENV;
		exp->type = EXP_TYPE_GET_PROPERTY;
		
		if(exp->slots.c < -MAX_GENERIC_CONST_INDEX){
			exp_xconst = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_XCONST, exp->token);
			exp_xconst->slots.b = exp->slots.c;
			exp_xconst->slots.a = exp->slots.c = exp->slots.a;
			return Lib::addXconst(this, exp, exp_xconst);
		}
		return exp;

	case EXP_TYPE_SET_ENV_VAR:
		OS_ASSERT(exp->list.count == 1);
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		OS_ASSERT(stack_pos < scope->function->stack_cur_size);
		exp->slots.a = scope->function->num_params + VAR_ENV;
		exp->slots.b = -1 - exp->slots.b - prog_numbers.count - CONST_STD_VALUES;
		exp->slots.c = --scope->function->stack_cur_size;
		exp->type = EXP_TYPE_SET_PROPERTY;

		if(exp->slots.b < -MAX_GENERIC_CONST_INDEX){
			exp_xconst = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_XCONST, exp->token);
			exp_xconst->slots.b = exp->slots.b;
			exp_xconst->slots.a = exp->slots.b = exp->slots.c + 1;
			if(exp_xconst->slots.a >= scope->function->stack_size){
				scope->function->stack_size = exp_xconst->slots.a + 1;
			}
			exp->list.add(exp_xconst OS_DBG_FILEPOS);
		}
		exp1 = exp->list[0];
		return exp;

	case EXP_TYPE_GET_PROPERTY_AUTO_CREATE:
		exp->type = EXP_TYPE_GET_PROPERTY;
		// no break

	case EXP_TYPE_GET_PROPERTY:
		OS_ASSERT(exp->list.count == 2);
		stack_pos = scope->function->stack_cur_size;
		exp = Lib::processList(this, scope, exp);
		OS_ASSERT(stack_pos+2 == scope->function->stack_cur_size);
		exp1 = exp->list[0];
		exp2 = exp->list[1];
		exp->slots.a = stack_pos;
		exp->slots.b = stack_pos; // exp1->slots.a;
		exp->slots.c = stack_pos + 1; // exp2->slots.a;
		scope->popTempVar();
		if(exp1->type == EXP_TYPE_MOVE){
			exp->slots.b = exp1->slots.b;
			exp1->type = EXP_TYPE_NOP;
		}
		if(exp2->type == EXP_TYPE_MOVE){
			exp->slots.c = exp2->slots.b;
			exp2->type = EXP_TYPE_NOP;
		}
		return exp;
	}
	return Lib::processList(this, scope, exp);
}


#if 0 // speed optimization
OS::Core::Compiler::Expression * OS::Core::Compiler::postCompilePass3(Scope * scope, Expression * exp)
{
	struct Lib {
		static Expression * processList(Compiler * compiler, Scope * scope, Expression * exp)
		{
			for(int i = 0; i < exp->list.count; i++){
				exp->list[i] = compiler->postCompilePass3(scope, exp->list[i]);
			}
			return exp;
		}
	};
	Expression * exp1, * exp2;
	switch(exp->type){
	case EXP_TYPE_FUNCTION:
		{
			Scope * new_scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(new_scope && (new_scope->parent == scope || (!new_scope->parent && new_scope->type == EXP_TYPE_FUNCTION)));
			scope = new_scope;
			break;
		}

	case EXP_TYPE_SCOPE:
	case EXP_TYPE_LOOP_SCOPE:
		{
			Scope * new_scope = dynamic_cast<Scope*>(exp);
			OS_ASSERT(new_scope && (new_scope->parent == scope || (!new_scope->parent && new_scope->type == EXP_TYPE_FUNCTION)));
			scope = new_scope;
			break;
		}

	case EXP_TYPE_GET_PROPERTY:
		{
			OS_ASSERT(exp->list.count == 2);
			exp = Lib::processList(this, scope, exp);
			exp1 = exp->list[0];
			exp2 = exp->list[1];
			if(exp1->type == EXP_TYPE_GET_LOCAL_VAR && exp2->type == EXP_TYPE_GET_LOCAL_VAR
				&& !exp1->local_var.up_count && exp1->local_var.index <= 255
				&& !exp2->local_var.up_count && exp2->local_var.index <= 255)
			{
				exp->type = EXP_TYPE_GET_PROPERTY_BY_LOCALS;
			}else if(exp1->type == EXP_TYPE_GET_LOCAL_VAR && exp2->type == EXP_TYPE_CONST_NUMBER
				&& !exp1->local_var.up_count && exp1->local_var.index <= 255)
			{
				exp->type = EXP_TYPE_GET_PROPERTY_BY_LOCAL_AND_NUMBER;
			}else if(exp1->type == EXP_TYPE_GET_THIS && exp2->type == EXP_TYPE_CONST_STRING){
				exp->type = EXP_TYPE_GET_THIS_PROPERTY_BY_STRING;
			}
			return exp;
		}

	case EXP_TYPE_SET_PROPERTY:
		{
			OS_ASSERT(exp->list.count == 3);
			exp = Lib::processList(this, scope, exp);
			exp1 = exp->list[1];
			exp2 = exp->list[2];
			if(exp1->type == EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE && exp2->type == EXP_TYPE_GET_LOCAL_VAR
				&& !exp1->local_var.up_count && exp1->local_var.index <= 255
				&& !exp2->local_var.up_count && exp2->local_var.index <= 255)
			{
				if(exp->list[0]->type == EXP_TYPE_GET_PROPERTY_BY_LOCALS){
					exp->type = EXP_TYPE_GET_SET_PROPERTY_BY_LOCALS_AUTO_CREATE;
				}else{
					exp->type = EXP_TYPE_SET_PROPERTY_BY_LOCALS_AUTO_CREATE;
				}
			}
			return exp;
		}

	case EXP_TYPE_CONCAT:
	case EXP_TYPE_LOGIC_PTR_EQ:
	case EXP_TYPE_LOGIC_PTR_NE:
	case EXP_TYPE_LOGIC_EQ:
	case EXP_TYPE_LOGIC_NE:
	case EXP_TYPE_LOGIC_GE:
	case EXP_TYPE_LOGIC_LE:
	case EXP_TYPE_LOGIC_GREATER:
	case EXP_TYPE_LOGIC_LESS:
	case EXP_TYPE_BIT_AND:
	case EXP_TYPE_BIT_OR:
	case EXP_TYPE_BIT_XOR:
	case EXP_TYPE_ADD: // +
	case EXP_TYPE_SUB: // -
	case EXP_TYPE_MUL: // *
	case EXP_TYPE_DIV: // /
	case EXP_TYPE_MOD: // %
	case EXP_TYPE_LSHIFT: // <<
	case EXP_TYPE_RSHIFT: // >>
	case EXP_TYPE_POW: // **
		{
			OS_ASSERT(exp->list.count == 2);
			exp = Lib::processList(this, scope, exp);
			exp1 = exp->list[0];
			exp2 = exp->list[1];
			if(exp1->type == EXP_TYPE_GET_LOCAL_VAR && exp2->type == EXP_TYPE_GET_LOCAL_VAR
				&& !exp1->local_var.up_count && exp1->local_var.index <= 255
				&& !exp2->local_var.up_count && exp2->local_var.index <= 255)
			{
				exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_BIN_OPERATOR_BY_LOCALS, exp->token, exp OS_DBG_FILEPOS);
				exp->ret_values = exp->list[0]->ret_values;
			}else if(exp1->type == EXP_TYPE_GET_LOCAL_VAR && exp2->type == EXP_TYPE_CONST_NUMBER
				&& !exp1->local_var.up_count && exp1->local_var.index <= 255)
			{
				exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_BIN_OPERATOR_BY_LOCAL_AND_NUMBER, exp->token, exp OS_DBG_FILEPOS);
				exp->ret_values = exp->list[0]->ret_values;
			}
			return exp;
		}

	case EXP_TYPE_SET_LOCAL_VAR:
		{
			OS_ASSERT(exp->list.count == 1);
			exp = Lib::processList(this, scope, exp);
			if(!exp->local_var.up_count && exp->local_var.index <= 255){
				exp1 = exp->list[0];
				if(exp1->type == EXP_TYPE_BIN_OPERATOR_BY_LOCAL_AND_NUMBER){
					exp->type = EXP_TYPE_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCAL_AND_NUMBER;
				}else if(exp1->type == EXP_TYPE_BIN_OPERATOR_BY_LOCALS){
					exp->type = EXP_TYPE_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCALS;
				}
			}
			return exp;
		}

	case EXP_TYPE_POST_INC:
	case EXP_TYPE_POST_DEC:
	case EXP_TYPE_PRE_INC:
	case EXP_TYPE_PRE_DEC:
		OS_ASSERT(false);
		break;

	case EXP_TYPE_NAME:
		OS_ASSERT(false);
		break;

	case EXP_TYPE_CALL_DIM:
		OS_ASSERT(false);
		break;

	case EXP_TYPE_INDIRECT:
		OS_ASSERT(false);
		break;
	}
	return Lib::processList(this, scope, exp);
}
#endif

OS::Core::Compiler::Scope * OS::Core::Compiler::expectTextExpression()
{
	OS_ASSERT(recent_token);

	Scope * scope = new (malloc(sizeof(Scope) OS_DBG_FILEPOS)) Scope(NULL, EXP_TYPE_FUNCTION, recent_token);
	// scope->function = scope;
	scope->parser_started = true;
	scope->ret_values = 1;
	scope->addStdVars();

	Params p = Params()
		.setAllowAssign(true)
		.setAllowAutoCall(true)
		.setAllowBinaryOperator(true)
		.setAllowParams(true)
		.setAllowRootBlocks(true);

	Expression * exp;
	ExpressionList list(allocator);

	while(!isError()){
		exp = expectSingleExpression(scope, p);
		if(isError()){
			break;
		}
		if(exp){
			list.add(exp OS_DBG_FILEPOS);
		}
		if(!recent_token){
			break;
		}
		TokenType token_type = recent_token->type;
		if(token_type == Tokenizer::CODE_SEPARATOR){
			if(!readToken()){
				break;
			}
			token_type = recent_token->type;
		}
		if(token_type == Tokenizer::END_ARRAY_BLOCK 
			|| token_type == Tokenizer::END_BRACKET_BLOCK
			|| token_type == Tokenizer::END_CODE_BLOCK)
		{
			break;
		}
	}
	if(isError()){
		allocator->deleteObj(scope);
		return NULL;
	}
	if(recent_token){
		setError(ERROR_SYNTAX, recent_token);
		allocator->deleteObj(scope);
		return NULL;
	}
	if(list.count == 0){
		return scope;
	}
	int ret_values = list.count == 1 && list[0]->ret_values > 0 && list[0]->type == EXP_TYPE_FUNCTION ? 1 : 0;
	{
		putNextTokenType(Tokenizer::CODE_SEPARATOR);
		readToken();

		TokenData * name_token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), 
			allocator->core->strings->var_env, 
			Tokenizer::NAME, recent_token->line, recent_token->pos);

		ExpressionList& func_exp_list = ret_values == 1 ? list[0]->list : list;
		Expression * name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_LOCAL_VAR, name_token);
		name_exp->ret_values = 1;
		if(!findLocalVar(name_exp->local_var, scope, allocator->core->strings->var_env, scope->num_locals, false)){
			OS_ASSERT(false);
		}
		OS_ASSERT(name_exp->local_var.up_count == 0);
		Expression * ret_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_RETURN, recent_token, name_exp OS_DBG_FILEPOS);
		ret_exp->ret_values = 1;
		func_exp_list.add(ret_exp OS_DBG_FILEPOS);

		name_token->release();
	}
	exp = newExpressionFromList(list, ret_values);
	switch(exp->type){
	case EXP_TYPE_CODE_LIST:
		if(exp->list.count == 1 && exp->list[0]->type == EXP_TYPE_FUNCTION){
			allocator->deleteObj(scope);
			scope = dynamic_cast<Scope*>(exp->list[0]);
			allocator->vectorClear(exp->list);
			allocator->deleteObj(exp);
			return scope;
		}
		// exp = expectExpressionValues(exp, 0);
		scope->list.swap(exp->list);
		allocator->deleteObj(exp);
		break;

	case EXP_TYPE_FUNCTION:
		OS_ASSERT(scope->num_locals == 0);
		allocator->deleteObj(scope);
		scope = dynamic_cast<Scope*>(exp);
		OS_ASSERT(scope);
		scope->parent = NULL;
		return scope;

	default:
		scope->list.add(exp OS_DBG_FILEPOS);
	}
	return scope;
}

OS::Core::Compiler::Scope * OS::Core::Compiler::expectCodeExpression(Scope * parent)
{
	OS_ASSERT(recent_token && recent_token->type == Tokenizer::BEGIN_CODE_BLOCK);
	if(!expectToken()){
		allocator->deleteObj(parent);
		return NULL;
	}

	Scope * scope;
	bool is_new_func;
	if(parent->type == EXP_TYPE_FUNCTION && !parent->parser_started){
		scope = parent;
		is_new_func = true;
		parent->parser_started = true;
	}else{
		scope = new (malloc(sizeof(Scope) OS_DBG_FILEPOS)) Scope(parent, EXP_TYPE_SCOPE, recent_token);
		// scope->function = parent->function;
		is_new_func = false;
	}

	Params p = Params()
		.setAllowAssign(true)
		.setAllowAutoCall(true)
		.setAllowBinaryOperator(true)
		.setAllowParams(true)
		.setAllowRootBlocks(true);

	Expression * exp;
	ExpressionList list(allocator);
	while(!isError()){
		exp = expectSingleExpression(scope, p);
		if(isError()){
			break;
		}
		if(exp){
			list.add(exp OS_DBG_FILEPOS);
		}
		TokenType token_type = recent_token->type;
		if(token_type == Tokenizer::CODE_SEPARATOR){
			if(!readToken()){
				break;
			}
			token_type = recent_token->type;
		}
		if(token_type == Tokenizer::END_ARRAY_BLOCK 
			|| token_type == Tokenizer::END_BRACKET_BLOCK
			|| token_type == Tokenizer::END_CODE_BLOCK)
		{
			break;
		}
	}
	if(isError()){
		allocator->deleteObj(scope);
		return NULL;
	}
	if(!recent_token || recent_token->type != Tokenizer::END_CODE_BLOCK){
		setError(Tokenizer::END_CODE_BLOCK, recent_token);
		allocator->deleteObj(scope);
		return NULL;
	}
	readToken();

	if(list.count == 0){
		return scope;
	}
	if(is_new_func){
		exp = newExpressionFromList(list, 1, true);
	}else{
		exp = newExpressionFromList(list, 0);
	}
	switch(exp->type){
	case EXP_TYPE_CODE_LIST:
		{
			scope->list.swap(exp->list);
			allocator->deleteObj(exp);
			break;
		}
		// no break

	default:
		scope->list.add(exp OS_DBG_FILEPOS);
	}
	if(is_new_func && scope->list.count > 0){
		Expression * last_exp = scope->list.lastElement();
		if(last_exp->ret_values == 1 && last_exp->type != EXP_TYPE_RETURN){
			scope->list.lastElement() = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_RETURN, last_exp->token, last_exp OS_DBG_FILEPOS);
			scope->list.lastElement()->ret_values = 1;
		}
	}
	return scope;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectObjectOrFunctionExpression(Scope * scope, const Params& org_p, bool allow_finish_exp)
{
	OS_ASSERT(recent_token && recent_token->type == Tokenizer::BEGIN_CODE_BLOCK);
	if(!expectToken()){
		return NULL;
	}
	if(recent_token->type == Tokenizer::OPERATOR_BIT_OR){
		Expression * exp = expectFunctionSugarExpression(scope); // {|x, x2| x*x2}
		// TODO: process org_p, allow_finish_exp ?
		return exp;
	}
	ungetToken();
	struct Lib {
		Compiler * compiler;
		Expression * obj_exp;

		Expression * finishValue(Scope * scope, const Params& p, bool allow_finish_exp)
		{
			if(!allow_finish_exp){
				return obj_exp;
			}
			return compiler->finishValueExpression(scope, obj_exp, Params(p).setAllowAssign(false).setAllowAutoCall(false));
		}

		void * malloc(int size OS_DBG_FILEPOS_DECL)
		{
			return compiler->malloc(size OS_DBG_FILEPOS_PARAM);
		}

		Lib(Compiler * p_compiler, int active_locals)
		{
			compiler = p_compiler;
			obj_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_OBJECT, compiler->recent_token);
			obj_exp->ret_values = 1;
		}

		Expression * error()
		{
			compiler->allocator->deleteObj(obj_exp);
			return NULL;
		}

		Expression * error(ErrorType err, TokenData * token)
		{
			compiler->setError(err, token);
			return error();
		}

		Expression * error(TokenType err, TokenData * token)
		{
			compiler->setError(err, token);
			return error();
		}

	} lib(this, scope->function->num_locals);

	Params p = Params().setAllowBinaryOperator(true);

	// TokenData * name_token, * save_token;
	for(readToken();;){
		Expression * exp = NULL;
		if(!recent_token){
			return lib.error(ERROR_SYNTAX, recent_token);
		}
		if(recent_token->type == Tokenizer::END_CODE_BLOCK){
			readToken();
			return lib.finishValue(scope, org_p, allow_finish_exp);
		}
		TokenData * name_token = recent_token;
		if(name_token->type == Tokenizer::BEGIN_ARRAY_BLOCK){
			readToken();
			TokenData * save_token = recent_token;
			exp = expectSingleExpression(scope, p);
			if(!exp){
				return lib.error();
			}
			if(exp->ret_values < 1){
				allocator->deleteObj(exp);
				return lib.error(ERROR_EXPECT_VALUE, save_token);
			}
			exp = expectExpressionValues(exp, 1);
			if(!recent_token || recent_token->type != Tokenizer::END_ARRAY_BLOCK){
				allocator->deleteObj(exp);
				return lib.error(Tokenizer::END_ARRAY_BLOCK, recent_token);
			}
			if(!readToken() || (recent_token->type != Tokenizer::OPERATOR_COLON && recent_token->type != Tokenizer::OPERATOR_ASSIGN)){
				allocator->deleteObj(exp);
				return lib.error(Tokenizer::OPERATOR_COLON, recent_token);
			}
			save_token = readToken();
			Expression * exp2 = expectSingleExpression(scope, p);
			if(!exp2){
				return isError() ? lib.error() : lib.error(ERROR_EXPECT_EXPRESSION, save_token);
			}
			exp2 = expectExpressionValues(exp2, 1);
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_OBJECT_SET_BY_EXP, name_token, exp, exp2 OS_DBG_FILEPOS);
		}else if(isNextToken(Tokenizer::OPERATOR_COLON) || isNextToken(Tokenizer::OPERATOR_ASSIGN)){
			ExpressionType exp_type = EXP_TYPE_OBJECT_SET_BY_NAME;
			switch(name_token->type){
			case Tokenizer::STRING:
			case Tokenizer::NAME:
				break;

			case Tokenizer::NUMBER:
				if(name_token->getFloat() != (OS_FLOAT)(OS_INT)name_token->getFloat()){
					// use it as EXP_TYPE_OBJECT_SET_BY_NAME
					break;
				}
				exp_type = EXP_TYPE_OBJECT_SET_BY_INDEX;
				break;

			default:
				return lib.error(ERROR_SYNTAX, name_token);
			}
			readToken(); // skip OPERATOR_COLON
			TokenData * save_token = readToken();
			exp = expectSingleExpression(scope, p);
			if(!exp){
				return isError() ? lib.error() : lib.error(ERROR_EXPECT_EXPRESSION, save_token);
			}
			exp = expectExpressionValues(exp, 1);
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(exp_type, name_token, exp OS_DBG_FILEPOS);
		}else{
			exp = expectSingleExpression(scope, p);
			if(!exp){
				return isError() ? lib.error() : lib.error(ERROR_EXPECT_EXPRESSION, name_token);
			}
			exp = expectExpressionValues(exp, 1);
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_OBJECT_SET_BY_AUTO_INDEX, name_token, exp OS_DBG_FILEPOS);
		}
		OS_ASSERT(exp);
		lib.obj_exp->list.add(exp OS_DBG_FILEPOS);
		if(recent_token && recent_token->type == Tokenizer::END_CODE_BLOCK){
			readToken();
			return lib.finishValue(scope, org_p, allow_finish_exp);
		}
#if 11
		if(!recent_token){
			return lib.error(Tokenizer::END_CODE_BLOCK, recent_token);
		}
		switch(recent_token->type){
		case Tokenizer::PARAM_SEPARATOR:
		case Tokenizer::CODE_SEPARATOR:
			readToken();
		}
#else
		if(!recent_token || (recent_token->type != Tokenizer::PARAM_SEPARATOR
			&& recent_token->type != Tokenizer::CODE_SEPARATOR)){
				return lib.error(Tokenizer::PARAM_SEPARATOR, recent_token);
		}
		readToken();
#endif
	}
	return NULL; // shut up compiler
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectArrayExpression(Scope * scope, const Params& __p)
{
	Params next_p = Params(__p).setAllowAssign(false).setAllowAutoCall(false);
	Expression * params = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_ARRAY, recent_token);
	params->ret_values = 1;
	readToken();
	if(recent_token && recent_token->type == Tokenizer::END_ARRAY_BLOCK){
		readToken();
		return finishValueExpression(scope, params, next_p);
	}
	Params p = Params().setAllowBinaryOperator(true);
	for(;;){
		Expression * exp = expectSingleExpression(scope, p);
		if(!exp){
			if(isError()){
				allocator->deleteObj(params);
				return NULL;
			}
			if(!recent_token || recent_token->type != Tokenizer::END_ARRAY_BLOCK){
				setError(Tokenizer::END_ARRAY_BLOCK, recent_token);
				allocator->deleteObj(params);
				return NULL;
			}
			readToken();
			return finishValueExpression(scope, params, next_p);
		}
		exp = expectExpressionValues(exp, 1);
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_OBJECT_SET_BY_AUTO_INDEX, exp->token, exp OS_DBG_FILEPOS);
		params->list.add(exp OS_DBG_FILEPOS);
		if(recent_token && recent_token->type == Tokenizer::END_ARRAY_BLOCK){
			readToken();
			return finishValueExpression(scope, params, next_p);
		}
#if 11
		if(!recent_token){
			setError(Tokenizer::END_ARRAY_BLOCK, recent_token);
			allocator->deleteObj(params);
			return NULL;
		}
		switch(recent_token->type){
		case Tokenizer::PARAM_SEPARATOR:
		case Tokenizer::CODE_SEPARATOR:
			readToken();
		}
#else
		if(!recent_token || (recent_token->type != Tokenizer::PARAM_SEPARATOR
			&& recent_token->type != Tokenizer::CODE_SEPARATOR)){
				setError(Tokenizer::PARAM_SEPARATOR, recent_token);
				allocator->deleteObj(params);
				return NULL;
		}
		readToken();
#endif
	}
	return NULL; // shut up compiler
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectParamsExpression(Scope * scope)
{
	struct Lib 
	{
		static Expression * calcParamsExpression(Compiler * compiler, Scope * scope, Expression * params)
		{
			if(params->list.count > 1){
				for(int i = 0; i < params->list.count; i++){
					params->list[i] = compiler->expectExpressionValues(params->list[i], 1);
				}
				params->ret_values = params->list.count;
			}else if(params->list.count == 1){
				params->ret_values = params->list[0]->ret_values;
			}
			return params;
		}
	};

	// OS_ASSERT(recent_token->type == Tokenizer::PARAM_SEPARATOR);
	Expression * params = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, recent_token);
	bool is_dim = recent_token->type == Tokenizer::BEGIN_ARRAY_BLOCK;
	TokenType end_exp_type = is_dim ? Tokenizer::END_ARRAY_BLOCK : Tokenizer::END_BRACKET_BLOCK;
	readToken();
	if(recent_token && recent_token->type == end_exp_type){
		readToken();
		return Lib::calcParamsExpression(this, scope, params);
	}
	Params p = Params().setAllowBinaryOperator(true);
	for(;;){
		Expression * exp = expectSingleExpression(scope, p);
		if(!exp){
			if(isError()){
				allocator->deleteObj(params);
				return NULL;
			}
			if(!recent_token || recent_token->type != end_exp_type){
				setError(end_exp_type, recent_token);
				allocator->deleteObj(params);
				return NULL;
			}
			readToken();
			return Lib::calcParamsExpression(this, scope, params);
		}
		// exp = expectExpressionValues(exp, 1);
		params->list.add(exp OS_DBG_FILEPOS);
		// params->ret_values += exp->ret_values;
		if(recent_token && (recent_token->type == Tokenizer::PARAM_SEPARATOR || recent_token->type == Tokenizer::CODE_SEPARATOR)){
			readToken();
		}
		if(recent_token && recent_token->type == end_exp_type){
			readToken();
			return Lib::calcParamsExpression(this, scope, params);
		}
		if(!recent_token){ // || recent_token->type != Tokenizer::PARAM_SEPARATOR){
			// setError(Tokenizer::PARAM_SEPARATOR, recent_token);
			setError(end_exp_type, recent_token);
			allocator->deleteObj(params);
			return NULL;
		}
	}
	return NULL; // shut up compiler
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectBracketExpression(Scope * scope, const Params& p)
{
	OS_ASSERT(recent_token && recent_token->type == Tokenizer::BEGIN_BRACKET_BLOCK);
	readToken();
	Expression * exp = expectSingleExpression(scope, Params()
		.setAllowBinaryOperator(true)
		.setAllowCall(true)
		.setAllowAutoCall(true));
	if(!exp){
		return NULL;
	}
	exp = newSingleValueExpression(exp);
	OS_ASSERT(exp->ret_values == 1);
	if(!recent_token){
		setError(Tokenizer::END_BRACKET_BLOCK, recent_token);
		allocator->deleteObj(exp);
		return NULL;
	}
	switch(recent_token->type){
	case Tokenizer::END_BRACKET_BLOCK:
		readToken();
		return finishValueExpression(scope, exp, p);
	}
	setError(Tokenizer::END_BRACKET_BLOCK, recent_token);
	allocator->deleteObj(exp);
	return NULL;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectExtendsExpression(Scope * scope)
{
	OS_ASSERT(recent_token && recent_token->str == allocator->core->strings->syntax_extends);
	TokenData * save_token = recent_token;
	if(!expectToken()){
		return NULL;
	}
	Params p;
	Expression * exp = expectSingleExpression(scope, p);
	if(!exp){
		return NULL;
	}
	if(exp->type == EXP_TYPE_CALL_AUTO_PARAM){
		OS_ASSERT(exp->list.count == 2);
		Expression * params = exp->list[1];
		OS_ASSERT(params->type == EXP_TYPE_PARAMS && params->list.count == 1);
		exp->list[1] = params->list[0];
		allocator->vectorClear(params->list);
		allocator->deleteObj(params);
		exp->type = EXP_TYPE_EXTENDS;
		exp->ret_values = 1;
		return exp;
	}
	Expression * exp2 = expectSingleExpression(scope, p);
	if(!exp2){
		allocator->deleteObj(exp);
		return NULL;
	}
	exp = expectExpressionValues(exp, 1);
	exp2 = expectExpressionValues(exp2, 1);
	exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_EXTENDS, save_token, exp, exp2 OS_DBG_FILEPOS);
	exp->ret_values = 1;
	return exp;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::finishQuestionOperator(Scope * scope, TokenData * token, Expression * exp, Expression * exp2)
{
	// OS_ASSERT(recent_token && recent_token->type == Tokenizer::OPERATOR_COLON);
	ungetToken();
	if(!expectToken(Tokenizer::OPERATOR_COLON)){
		allocator->deleteObj(exp);
		allocator->deleteObj(exp2);
		return NULL;
	}
	if(!expectToken()){
		return NULL;
	}
	Expression * exp3 = expectSingleExpression(scope, Params().setAllowBinaryOperator(true));
	if(!exp3){
		allocator->deleteObj(exp);
		allocator->deleteObj(exp2);
		return NULL;
	}
	exp = expectExpressionValues(exp, 1);
	exp2 = expectExpressionValues(exp2, 1);
	exp3 = expectExpressionValues(exp3, 1);
	exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_QUESTION, token, exp, exp2, exp3 OS_DBG_FILEPOS);
	exp->ret_values = 1;
	return exp;
}

/*
OS::Core::Compiler::Expression * OS::Core::Compiler::expectCloneExpression(Scope * scope)
{
	OS_ASSERT(recent_token && recent_token->str == allocator->core->strings->syntax_clone);
	TokenData * save_token = recent_token;
	if(!expectToken()){
		return NULL;
	}
	Expression * exp = expectSingleExpression(scope, Params());
	if(!exp){
		return NULL;
	}
	exp = expectExpressionValues(exp, 1);
	exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CLONE, save_token, exp OS_DBG_FILEPOS);
	exp->ret_values = 1;
	return exp;
}
*/

OS::Core::Compiler::Expression * OS::Core::Compiler::expectDeleteExpression(Scope * scope)
{
	OS_ASSERT(recent_token && recent_token->str == allocator->core->strings->syntax_delete);
	TokenData * save_token = recent_token;
	if(!expectToken()){
		return NULL;
	}
	Expression * exp = expectSingleExpression(scope, Params());
	if(!exp){
		return NULL;
	}
	if(exp->type == EXP_TYPE_INDIRECT){
		OS_ASSERT(exp->list.count == 2);
		Expression * field = exp->list[1];
		if(field->type == EXP_TYPE_NAME){
			field->type = EXP_TYPE_CONST_STRING;
		}
		exp->type = EXP_TYPE_DELETE;
		exp->ret_values = 0;
		return exp;
	}
	if(exp->type == EXP_TYPE_CALL_DIM){
		OS_ASSERT(exp->list.count == 2);
		Expression * params = exp->list[1];
		if(params->list.count == 1){
			exp->list[1] = params->list[0];
			allocator->vectorClear(params->list);
			allocator->deleteObj(params);
			exp->type = EXP_TYPE_DELETE;
			exp->ret_values = 0;
			return exp;
		}
		Expression * object = exp->list[0];

		String method_name = !params->list.count ? allocator->core->strings->__delempty : allocator->core->strings->__deldim;
		TokenData * token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), method_name, Tokenizer::NAME, object->token->line, object->token->pos);
		Expression * exp_method_name = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_STRING, token);
		exp_method_name->ret_values = 1;
		token->release();

		Expression * indirect = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_INDIRECT, object->token, object, exp_method_name OS_DBG_FILEPOS);
		exp->list[0] = indirect;
		exp->type = EXP_TYPE_CALL;
		exp->ret_values = 1;
		return exp;
	}
	setError(ERROR_SYNTAX, exp->token);
	allocator->deleteObj(exp);
	return NULL;
}

/*
OS::Core::Compiler::Expression * OS::Core::Compiler::expectValueOfExpression(Scope * scope, ExpressionType exp_type)
{
	OS_ASSERT(recent_token);
	TokenData * save_token = recent_token;
	if(!expectToken()){
		return NULL;
	}
	Expression * exp = expectSingleExpression(scope, Params());
	if(!exp){
		return NULL;
	}
	exp = expectExpressionValues(exp, 1);
	exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(exp_type, save_token, exp OS_DBG_FILEPOS);
	exp->ret_values = 1;
	return exp;
}
*/

OS::Core::Compiler::Expression * OS::Core::Compiler::expectFunctionExpression(Scope * parent)
{
	Scope * scope = new (malloc(sizeof(Scope) OS_DBG_FILEPOS)) Scope(parent, EXP_TYPE_FUNCTION, recent_token);
	scope->function = scope;
	scope->ret_values = 1;
	Expression * name_exp = NULL;
	if(isNextToken(Tokenizer::NAME)){
		TokenData * token = readToken();
		if(isNextToken(Tokenizer::NAME)){
			String prefix(allocator);
			if(token->str == allocator->core->strings->syntax_get){
				prefix = allocator->core->strings->__getAt;
			}else if(token->str == allocator->core->strings->syntax_set){
				prefix = allocator->core->strings->__setAt;
			}else{
				setError(ERROR_EXPECT_GET_OR_SET, token);
				allocator->deleteObj(name_exp);
				allocator->deleteObj(scope);
				return NULL;
			}
			token = readToken();
			token->str = String(allocator, prefix, token->str);
			name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, token);
		}else{
			name_exp = expectSingleExpression(parent, Params().setAllowCall(false));
			if(!name_exp || !name_exp->isWriteable()){
				setError(ERROR_EXPECT_WRITEABLE, token);
				allocator->deleteObj(name_exp);
				allocator->deleteObj(scope);
				return NULL;
			}
			ungetToken();
		}
	}
	if(!expectToken(Tokenizer::BEGIN_BRACKET_BLOCK)){
		allocator->deleteObj(scope);
		return NULL;
	}
	for(;;){
		if(!readToken()){
			setError(ERROR_SYNTAX, recent_token);
			allocator->deleteObj(scope);
			return NULL;
		}
		switch(recent_token->type){
		case Tokenizer::END_BRACKET_BLOCK:
			break;

		case Tokenizer::NAME:
			// scope->list.add(new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, recent_token));
			scope->addLocalVar(recent_token->str);
			scope->num_params++;
			if(!readToken()){
				setError(ERROR_SYNTAX, recent_token);
				allocator->deleteObj(scope);
				return NULL;
			}
			if(recent_token->type == Tokenizer::END_BRACKET_BLOCK){
				break;
			}
			if(recent_token->type == Tokenizer::PARAM_SEPARATOR){
				continue;
			}
#if 11
			ungetToken();
			continue;
#else
			setError(ERROR_SYNTAX, recent_token);
			allocator->deleteObj(scope);
			return NULL;
#endif

		default:
			setError(ERROR_SYNTAX, recent_token);
			allocator->deleteObj(scope);
			return NULL;
		}
		break;
	}
	OS_ASSERT(recent_token && recent_token->type == Tokenizer::END_BRACKET_BLOCK);
	if(!expectToken(Tokenizer::BEGIN_CODE_BLOCK)){
		allocator->deleteObj(scope);
		return NULL;
	}
	scope->addStdVars();
	scope = expectCodeExpression(scope);
	if(!scope || !name_exp){
		return scope;
	}
	return newBinaryExpression(parent, EXP_TYPE_ASSIGN, name_exp->token, name_exp, scope);
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectFunctionSugarExpression(Scope * parent)
{
	OS_ASSERT(recent_token && recent_token->type == Tokenizer::OPERATOR_BIT_OR);
	Scope * scope = new (malloc(sizeof(Scope) OS_DBG_FILEPOS)) Scope(parent, EXP_TYPE_FUNCTION, recent_token);
	scope->function = scope;
	scope->ret_values = 1;
	for(;;){
		if(!readToken()){
			setError(ERROR_SYNTAX, recent_token);
			allocator->deleteObj(scope);
			return NULL;
		}
		switch(recent_token->type){
		case Tokenizer::OPERATOR_BIT_OR:
			break;

		case Tokenizer::NAME:
			// scope->list.add(new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, recent_token));
			scope->addLocalVar(recent_token->str);
			scope->num_params++;
			if(!readToken()){
				setError(ERROR_SYNTAX, recent_token);
				allocator->deleteObj(scope);
				return NULL;
			}
			if(recent_token->type == Tokenizer::OPERATOR_BIT_OR){
				break;
			}
			if(recent_token->type == Tokenizer::PARAM_SEPARATOR){
				continue;
			}
#if 11
			ungetToken();
			continue;
#else
			setError(ERROR_SYNTAX, recent_token);
			allocator->deleteObj(scope);
			return NULL;
#endif

		default:
			setError(ERROR_SYNTAX, recent_token);
			allocator->deleteObj(scope);
			return NULL;
		}
		break;
	}
	OS_ASSERT(recent_token && recent_token->type == Tokenizer::OPERATOR_BIT_OR);
	scope->addStdVars();
	// scope = expectCodeExpression(scope);

	if(!expectToken()){
		allocator->deleteObj(scope);
		return NULL;
	}
	scope->parser_started = true;

	Params p = Params()
		.setAllowAssign(true)
		.setAllowAutoCall(true)
		.setAllowBinaryOperator(true)
		.setAllowParams(true)
		.setAllowRootBlocks(true);

	Expression * exp;
	ExpressionList list(allocator);
	while(!isError()){
		exp = expectSingleExpression(scope, p);
		if(isError()){
			break;
		}
		if(exp){
			list.add(exp OS_DBG_FILEPOS);
		}
		TokenType token_type = recent_token->type;
		if(token_type == Tokenizer::CODE_SEPARATOR){
			if(!readToken()){
				break;
			}
			token_type = recent_token->type;
		}
		if(token_type == Tokenizer::END_ARRAY_BLOCK 
			|| token_type == Tokenizer::END_BRACKET_BLOCK
			|| token_type == Tokenizer::END_CODE_BLOCK)
		{
			break;
		}
	}
	if(isError()){
		allocator->deleteObj(scope);
		return NULL;
	}
	if(!recent_token || recent_token->type != Tokenizer::END_CODE_BLOCK){
		setError(Tokenizer::END_CODE_BLOCK, recent_token);
		allocator->deleteObj(scope);
		return NULL;
	}
	readToken();

	if(list.count == 0){
		return scope;
	}
	exp = newExpressionFromList(list, 1, true);
	switch(exp->type){
	case EXP_TYPE_CODE_LIST:
		scope->list.swap(exp->list);
		allocator->deleteObj(exp);
		break;

	default:
		scope->list.add(exp OS_DBG_FILEPOS);
	}
	if(scope->list.count > 0){
		Expression * last_exp = scope->list.lastElement();
		if(last_exp->ret_values == 1){
			OS_ASSERT(last_exp->type != EXP_TYPE_RETURN);
			scope->list.lastElement() = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_RETURN, last_exp->token, last_exp OS_DBG_FILEPOS);
			scope->list.lastElement()->ret_values = 1;
		}
	}
	return scope;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectVarExpression(Scope * scope)
{
	OS_ASSERT(recent_token && recent_token->str == allocator->core->strings->syntax_var);
	if(!expectToken(Tokenizer::NAME)){
		return NULL;
	}
	Expression * name_exp;
	Expression * exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, recent_token);
	exp->ret_values = 1;
	if(recent_token->str == allocator->core->strings->syntax_function){
		if(!expectToken(Tokenizer::NAME)){
			allocator->deleteObj(exp);
			return NULL;
		}
		TokenData * name_token;
		if(recent_token->str == allocator->core->strings->syntax_get || recent_token->str == allocator->core->strings->syntax_set){
			bool is_getter = recent_token->str == allocator->core->strings->syntax_get;
			if(!expectToken(Tokenizer::NAME)){
				allocator->deleteObj(exp);
				return NULL;
			}
			if(!isVarNameValid(recent_token->str)){
				setError(ERROR_VAR_NAME, recent_token);
				allocator->deleteObj(exp);
				return NULL;
			}
			if(!expectToken(Tokenizer::BEGIN_BRACKET_BLOCK)){
				allocator->deleteObj(exp);
				return NULL;
			}
			ungetToken();
			ungetToken();

			name_token = tokenizer->removeToken(next_token_index-1); name_token->release();
			name_token = tokenizer->removeToken(next_token_index-1);
			//name_token->str = 
			if(is_getter){
				name_token->str = String(allocator, allocator->core->strings->__getAt, name_token->str);
			}else{
				name_token->str = String(allocator, allocator->core->strings->__setAt, name_token->str);
			}
		}else{
			if(!isVarNameValid(recent_token->str)){
				setError(ERROR_VAR_NAME, recent_token);
				allocator->deleteObj(exp);
				return NULL;
			}
			if(!expectToken(Tokenizer::BEGIN_BRACKET_BLOCK)){
				allocator->deleteObj(exp);
				return NULL;
			}
			ungetToken();

			name_token = tokenizer->removeToken(next_token_index-1);
		}

		name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, name_token);
		name_exp->ret_values = 1;
		name_token->release();

		allocator->deleteObj(exp);
		// exp->list.add(name_exp OS_DBG_FILEPOS);
		// exp->ret_values++;

		ungetToken(); // return to function

		Expression * func_exp = expectFunctionExpression(scope);
		if(!func_exp){
			allocator->deleteObj(exp);
			return NULL;
		}
		OS_ASSERT(func_exp->type == EXP_TYPE_FUNCTION);
		exp = newBinaryExpression(scope, EXP_TYPE_ASSIGN, name_exp->token, name_exp, func_exp);
	}else{
		for(;;){
			if(!isVarNameValid(recent_token->str)){
				setError(ERROR_VAR_NAME, recent_token);
				allocator->deleteObj(exp);
				return NULL;
			}
			name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, recent_token);
			name_exp->ret_values = 1;

			exp->list.add(name_exp OS_DBG_FILEPOS);
			exp->ret_values++;

			if(!readToken() || recent_token->type != Tokenizer::PARAM_SEPARATOR){
				break;
			}
			if(!expectToken(Tokenizer::NAME)){
				allocator->deleteObj(exp);
				return NULL;
			}
		}

		/*
		while(readToken()){
		if(recent_token->type != Tokenizer::PARAM_SEPARATOR){
		break;
		}
		if(!expectToken(Tokenizer::NAME)){
		allocator->deleteObj(exp);
		return NULL;
		}
		if(!isVarNameValid(recent_token->str)){
		setError(ERROR_VAR_NAME, recent_token);
		allocator->deleteObj(exp);
		return NULL;
		}
		name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, recent_token);
		name_exp->ret_values = 1;

		exp->list.add(name_exp OS_DBG_FILEPOS);
		exp->ret_values++;
		}
		*/
		if(recent_token && recent_token->type == Tokenizer::OPERATOR_ASSIGN){
			bool is_finished;
			exp = finishBinaryOperator(scope, getOpcodeLevel(exp->type), exp, Params().setAllowParams(true).setAllowInOperator(true), is_finished);
			OS_ASSERT(is_finished);
		}
	}
	// Expression * exp = expectSingleExpression(scope, Params().setAllowParams(true).setAllowAssign(true)); // false, true, false, true, false);
	Expression * ret_exp = exp;
	while(exp){
		switch(exp->type){
		case EXP_TYPE_PARAMS:
			{
				Expression * params = exp;
				for(int i = 0; i < params->list.count; i++){
					exp = params->list[i];
					OS_ASSERT(exp->type == EXP_TYPE_NAME);
					if(exp->type == EXP_TYPE_NAME){
						if(findLocalVar(exp->local_var, scope, exp->token->str, exp->active_locals, false)){
							// setError(ERROR_VAR_ALREADY_EXIST, exp->token);
							// allocator->deleteObj(ret_exp);
							// return NULL;
							// OS_ASSERT(true);
						}else{
							scope->addLocalVar(exp->token->str, exp->local_var);
						}
						OS_ASSERT(exp->local_var.up_count == 0);
						exp->type = EXP_TYPE_NEW_LOCAL_VAR;
						exp->ret_values = 0;
					}
				}
				params->ret_values = 0;
				return params;
			}

		case EXP_TYPE_SET_LOCAL_VAR:
			for(;;){
				if(exp->local_var.up_scope_count == 0){
					// setError(ERROR_VAR_ALREADY_EXIST, exp->token);
					// allocator->deleteObj(ret_exp);
					// return NULL;
					OS_ASSERT(true);
				}else{
					OS_ASSERT(!findLocalVar(exp->local_var, scope, exp->token->str, exp->active_locals, false));
					scope->addLocalVar(exp->token->str, exp->local_var);
				}
				OS_ASSERT(exp->list.count == 1);
				exp = exp->list[0];
				switch(exp->type){
				case EXP_TYPE_SET_ENV_VAR:
				case EXP_TYPE_SET_LOCAL_VAR:
					break;

				default:
					return ret_exp;
				}
				break;
			}
			break;

		case EXP_TYPE_SET_ENV_VAR:
			for(;;){
				OS_ASSERT(!findLocalVar(exp->local_var, scope, exp->token->str, exp->active_locals, false));
				scope->addLocalVar(exp->token->str, exp->local_var);
				exp->type = EXP_TYPE_SET_LOCAL_VAR;
				OS_ASSERT(exp->list.count == 1);
				exp = exp->list[0];
				switch(exp->type){
				case EXP_TYPE_SET_ENV_VAR:
				case EXP_TYPE_SET_LOCAL_VAR:
					break;

				default:
					return ret_exp;
				}
				break;
			}
			break;

		case EXP_TYPE_NAME:
			if(findLocalVar(exp->local_var, scope, exp->token->str, exp->active_locals, false)){
				// setError(ERROR_VAR_ALREADY_EXIST, exp->token);
				// allocator->deleteObj(ret_exp);
				// return NULL;
				OS_ASSERT(true);
			}else{
				scope->addLocalVar(exp->token->str, exp->local_var);
			}
			OS_ASSERT(exp->local_var.up_count == 0);
			exp->type = EXP_TYPE_NEW_LOCAL_VAR;
			exp->ret_values = 0;
			return ret_exp;

		default:
			return ret_exp;
		}
	}
	return ret_exp;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectForExpression(Scope * parent)
{
	OS_ASSERT(recent_token && recent_token->str == allocator->core->strings->syntax_for);

	Scope * scope = new (malloc(sizeof(Scope) OS_DBG_FILEPOS)) Scope(parent, EXP_TYPE_SCOPE, recent_token);
	if(!expectToken(Tokenizer::BEGIN_BRACKET_BLOCK) || !expectToken()){
		allocator->deleteObj(scope);
		return NULL;
	}
	// Expression * exp = expectSingleExpression(scope, true); // , true, true, true, true, true);
	Expression * exp = expectSingleExpression(scope, Params()
		.setAllowAssign(true)
		.setAllowAutoCall(true)
		.setAllowBinaryOperator(true)
		.setAllowParams(true)
		.setAllowVarDecl(true)
		.setAllowNopResult(true)
		.setAllowInOperator(false));

	if(!exp){
		allocator->deleteObj(scope);
		return NULL;
	}
	if(!recent_token){
		setError(ERROR_EXPECT_TOKEN, recent_token);
		allocator->deleteObj(scope);
		allocator->deleteObj(exp);
		return NULL;
	}	
	if(recent_token->type == Tokenizer::NAME && (exp->type == EXP_TYPE_PARAMS || exp->type == EXP_TYPE_NEW_LOCAL_VAR || exp->type == EXP_TYPE_NAME)){
		if(recent_token->str != allocator->core->strings->syntax_in){
			setError(allocator->core->strings->syntax_in, recent_token);
			allocator->deleteObj(scope);
			allocator->deleteObj(exp);
			return NULL;
		}
		ExpressionList vars(allocator);
		if(exp->type == EXP_TYPE_PARAMS){
			vars.swap(exp->list);
			allocator->deleteObj(exp);
		}else{
			vars.add(exp OS_DBG_FILEPOS);
		}
		exp = NULL;
		for(int i = 0; i < vars.count; i++){
			OS_ASSERT(vars[i]->type == EXP_TYPE_NAME || vars[i]->type == EXP_TYPE_NEW_LOCAL_VAR);
			Expression * name_exp = vars[i];
			if(name_exp->type == EXP_TYPE_NAME){
				/*
				scope->addLocalVar(name_exp->token->str, name_exp->local_var);
				OS_ASSERT(scope->function);
				name_exp->active_locals = scope->function->num_locals;
				name_exp->type = EXP_TYPE_NEW_LOCAL_VAR;
				*/
				name_exp->type = EXP_TYPE_NOP;
			}
		}
		if(!expectToken()){
			allocator->deleteObj(scope);
			return NULL;
		}
		exp = expectSingleExpression(scope, Params().setAllowBinaryOperator(true).setAllowAutoCall(true)); // true, false, false, false, true);
		if(!recent_token || recent_token->type != Tokenizer::END_BRACKET_BLOCK){
			setError(Tokenizer::END_BRACKET_BLOCK, recent_token);
			allocator->deleteObj(scope);
			allocator->deleteObj(exp);
			return NULL;
		}
		if(!exp->ret_values){
			setError(ERROR_EXPECT_VALUE, exp->token);
			allocator->deleteObj(scope);
			allocator->deleteObj(exp);
			return NULL;
		}
		exp = expectExpressionValues(exp, 1);
		if(!expectToken()){
			allocator->deleteObj(scope);
			allocator->deleteObj(exp);
			return NULL;
		}
		Scope * loop_scope = new (malloc(sizeof(Scope) OS_DBG_FILEPOS)) Scope(scope, EXP_TYPE_LOOP_SCOPE, recent_token);
		Expression * body_exp = expectSingleExpression(loop_scope, true, true);
		/* if(recent_token->type == Tokenizer::BEGIN_CODE_BLOCK){
		body_exp = expectCodeExpression(loop_scope);
		}else{
		body_exp = expectSingleExpression(loop_scope, true);
		} */
		if(!body_exp){
			allocator->deleteObj(scope);
			allocator->deleteObj(exp);
			allocator->deleteObj(loop_scope);
			return NULL;
		}
		body_exp = expectExpressionValues(body_exp, 0);

		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CALL_METHOD, exp->token, exp OS_DBG_FILEPOS);
		{
			Expression * params = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, exp->token);

			String method_name = allocator->core->strings->__iter;
			TokenData * token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), method_name, Tokenizer::NAME, exp->token->line, exp->token->pos);
			Expression * exp_method_name = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_STRING, token);
			exp_method_name->ret_values = 1;
			token->release();

			params->list.add(exp_method_name OS_DBG_FILEPOS);
			params->ret_values = 1;
			exp->list.add(params OS_DBG_FILEPOS);
		}
		exp = expectExpressionValues(exp, vars.count + 1);
		int num_locals = vars.count;

		// ExpressionList temp_vars(allocator);
		const int temp_count = 2;
		const OS_CHAR * temp_names[temp_count] = {
			OS_TEXT("#func"), /*OS_TEXT("#state"), OS_TEXT("#state2"), */ OS_TEXT("#valid")
		};
		for(int i = 0; i < temp_count; i++){
			String name(allocator, temp_names[i]);
			TokenData * token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), name, Tokenizer::NAME, exp->token->line, exp->token->pos);
			Expression * name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NEW_LOCAL_VAR, token);
			// name_exp->ret_values = 1;
			vars.add(name_exp OS_DBG_FILEPOS);
			token->release();

			scope->addLocalVar(name, name_exp->local_var);
			OS_ASSERT(scope->function);
			name_exp->active_locals = scope->function->num_locals;
			name_exp->local_var.type = LOCAL_TEMP;
		}

		ExpressionList list(allocator);

		// var func, state, state2 = (in_exp).__iter()
		{
			Expression * params = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, exp->token);
			for(int i = num_locals; i < vars.count-1; i++){
				Expression * var_exp = vars[i];
				Expression * name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, var_exp->token);
				OS_ASSERT(scope->function);
				name_exp->active_locals = scope->function->num_locals;
				name_exp->ret_values = 1;
				params->list.add(name_exp OS_DBG_FILEPOS);
			}
			params->ret_values = params->list.count;

			String assing_operator(allocator, OS_TEXT("="));
			TokenData * assign_token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), assing_operator, Tokenizer::OPERATOR_ASSIGN, exp->token->line, exp->token->pos);
			exp = newBinaryExpression(scope, EXP_TYPE_ASSIGN, assign_token, params, exp);
			OS_ASSERT(exp && exp->type == EXP_TYPE_SET_LOCAL_VAR && !exp->ret_values);
			assign_token->release();

			list.add(exp OS_DBG_FILEPOS); exp = NULL;
		}
		/*
		begin loop
		var valid, k, v = func(state, state2)
		if(!valid) break

		body_exp

		end loop
		*/
		list.add(loop_scope OS_DBG_FILEPOS);
		{
			// var valid, k, v
			Expression * params = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, loop_scope->token);
			for(int i = 0; i < num_locals+1; i++){
				Expression * var_exp = !i ? vars.lastElement() : vars[i-1];
				Expression * name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, var_exp->token);
				OS_ASSERT(scope->function);
				name_exp->active_locals = scope->function->num_locals;
				name_exp->ret_values = 1;
				params->list.add(name_exp OS_DBG_FILEPOS);
			}
			params->ret_values = params->list.count;

			// func(state, state2)
			Expression * call_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CALL, loop_scope->token);
			{
				Expression * var_exp = vars[num_locals]; // func
				Expression * name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, var_exp->token);
				OS_ASSERT(scope->function);
				name_exp->active_locals = scope->function->num_locals;
				name_exp->ret_values = 1;
				call_exp->list.add(name_exp OS_DBG_FILEPOS);

				Expression * params = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, loop_scope->token);
				for(int i = num_locals+1; i < vars.count-1; i++){
					Expression * var_exp = vars[i];
					Expression * name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, var_exp->token);
					OS_ASSERT(scope->function);
					name_exp->active_locals = scope->function->num_locals;
					name_exp->ret_values = 1;
					params->list.add(name_exp OS_DBG_FILEPOS);
				}
				params->ret_values = params->list.count;
				call_exp->list.add(params OS_DBG_FILEPOS);
			}
			call_exp->ret_values = params->list.count;

			// var valid, k, v = func(state, state2)
			String assing_operator(allocator, OS_TEXT("="));
			TokenData * assign_token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(tokenizer->getTextData(), assing_operator, 
				Tokenizer::OPERATOR_ASSIGN, loop_scope->token->line, loop_scope->token->pos);
			exp = newBinaryExpression(scope, EXP_TYPE_ASSIGN, assign_token, params, call_exp);
			OS_ASSERT(exp && exp->type == EXP_TYPE_SET_LOCAL_VAR && !exp->ret_values);
			assign_token->release();

			loop_scope->list.add(exp OS_DBG_FILEPOS); exp = NULL;
		}

		// if(!valid) break
		{
			Expression * var_exp = vars.lastElement(); // valid var
			Expression * name_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, var_exp->token);
			OS_ASSERT(scope->function);
			name_exp->active_locals = scope->function->num_locals;
			name_exp->ret_values = 1;

			Expression * not_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_LOGIC_NOT, loop_scope->token, name_exp OS_DBG_FILEPOS);
			not_exp->ret_values = 1;

			Expression * break_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_BREAK, loop_scope->token);
			Expression * if_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_IF, loop_scope->token, not_exp, break_exp OS_DBG_FILEPOS);
			loop_scope->list.add(if_exp OS_DBG_FILEPOS);
		}
		loop_scope->list.add(body_exp OS_DBG_FILEPOS);

		// assemble all exps
		scope->list.swap(vars);
		scope->list.add(newExpressionFromList(list, 0) OS_DBG_FILEPOS);
		return scope;
	}
	Expression * pre_exp = exp;
	if(recent_token->type != Tokenizer::CODE_SEPARATOR){
		setError(Tokenizer::CODE_SEPARATOR, recent_token);
		allocator->deleteObj(scope);
		allocator->deleteObj(pre_exp);
		return NULL;
	}
	readToken();
	Expression * bool_exp;
	if(recent_token->type == Tokenizer::CODE_SEPARATOR){
		bool_exp = NULL;
	}else{
		bool_exp = expectSingleExpression(scope, Params().setAllowAutoCall(true).setAllowBinaryOperator(true));
		if(!bool_exp){
			allocator->deleteObj(scope);
			allocator->deleteObj(pre_exp);
			return NULL;
		}
	}
	if(bool_exp && !bool_exp->ret_values){
		setError(ERROR_EXPECT_VALUE, bool_exp->token);
		allocator->deleteObj(scope);
		allocator->deleteObj(pre_exp);
		allocator->deleteObj(bool_exp);
		return NULL;
	}
	if(recent_token->type != Tokenizer::CODE_SEPARATOR){
		setError(Tokenizer::CODE_SEPARATOR, recent_token);
		allocator->deleteObj(scope);
		allocator->deleteObj(pre_exp);
		allocator->deleteObj(bool_exp);
		return NULL;
	}
	readToken();
	Expression * post_exp = expectSingleExpression(scope, Params()
		.setAllowAssign(true)
		.setAllowAutoCall(true)
		.setAllowBinaryOperator(true)
		.setAllowNopResult(true));
	if(!post_exp){
		allocator->deleteObj(scope);
		allocator->deleteObj(pre_exp);
		allocator->deleteObj(bool_exp);
		return NULL;
	}
	if(recent_token->type != Tokenizer::END_BRACKET_BLOCK){
		setError(Tokenizer::END_BRACKET_BLOCK, recent_token);
		allocator->deleteObj(scope);
		allocator->deleteObj(pre_exp);
		allocator->deleteObj(bool_exp);
		allocator->deleteObj(post_exp);
		return NULL;
	}
	readToken();

	Scope * loop_scope = new (malloc(sizeof(Scope) OS_DBG_FILEPOS)) Scope(scope, EXP_TYPE_LOOP_SCOPE, recent_token);
	Expression * body_exp = expectSingleExpression(loop_scope, true, true);
	if(!body_exp){
		allocator->deleteObj(scope);
		allocator->deleteObj(pre_exp);
		allocator->deleteObj(bool_exp);
		allocator->deleteObj(post_exp);
		allocator->deleteObj(loop_scope);
		return NULL;
	}
	if(bool_exp){
		bool_exp = expectExpressionValues(bool_exp, 1);
		Expression * not_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_LOGIC_NOT, bool_exp->token, bool_exp OS_DBG_FILEPOS);
		not_exp->ret_values = 1;

		Expression * break_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_BREAK, bool_exp->token);
		Expression * if_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_IF, bool_exp->token, not_exp, break_exp OS_DBG_FILEPOS);

		loop_scope->list.add(if_exp OS_DBG_FILEPOS);
	}
	body_exp = expectExpressionValues(body_exp, 0);
	loop_scope->list.add(body_exp OS_DBG_FILEPOS);

	post_exp = expectExpressionValues(post_exp, 0);
	loop_scope->list.add(post_exp OS_DBG_FILEPOS);

	scope->list.add(pre_exp OS_DBG_FILEPOS);
	scope->list.add(loop_scope OS_DBG_FILEPOS);
	return scope;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectDebugLocalsExpression(Scope * scope)
{
	OS_ASSERT(recent_token && recent_token->str == allocator->core->strings->syntax_debuglocals);

	Expression * exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_DEBUG_LOCALS, recent_token);
	exp->ret_values = 1;
	readToken();
	return exp;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectIfExpression(Scope * scope)
{
	OS_ASSERT(recent_token && (recent_token->str == allocator->core->strings->syntax_if 
		|| recent_token->str == allocator->core->strings->syntax_elseif));
	if(!expectToken(Tokenizer::BEGIN_BRACKET_BLOCK) || !expectToken()){
		return NULL;
	}
	TokenData * token = recent_token;
	Expression * if_exp = expectSingleExpression(scope, Params().setAllowBinaryOperator(true).setAllowNopResult(true));
	if(!if_exp){
		return NULL;
	}
	if(if_exp->ret_values < 1){
		setError(ERROR_EXPECT_VALUE, token);
		allocator->deleteObj(if_exp);
		return NULL;
	}
	if_exp = expectExpressionValues(if_exp, 1);
	if(!recent_token || recent_token->type != Tokenizer::END_BRACKET_BLOCK){
		setError(Tokenizer::END_BRACKET_BLOCK, recent_token);
		allocator->deleteObj(if_exp);
		return NULL;
	}
	if(!expectToken()){
		allocator->deleteObj(if_exp);
		return NULL;
	}
	if(!recent_token){
		setError(ERROR_EXPECT_TOKEN, recent_token);
		allocator->deleteObj(if_exp);
		return NULL;
	}
	token = recent_token;
	Expression * then_exp; // = expectSingleExpression(scope, true, true);
	if(recent_token->type == Tokenizer::BEGIN_CODE_BLOCK){
		then_exp = expectCodeExpression(scope);
	}else{
		then_exp = expectSingleExpression(scope, true);
		if(recent_token && recent_token->type == Tokenizer::CODE_SEPARATOR){
			readToken();
		}
	}
	if(!then_exp){
		allocator->deleteObj(if_exp);
		return NULL;
	}
	then_exp = expectExpressionValues(then_exp, 0);
	if(recent_token && recent_token->type == Tokenizer::NAME){
		Expression * else_exp = NULL;
		if(recent_token->str == allocator->core->strings->syntax_elseif){
			if(!expectToken()){
				allocator->deleteObj(if_exp);
				allocator->deleteObj(then_exp);
				return NULL;
			}
			else_exp = expectIfExpression(scope);
		}else if(recent_token->str == allocator->core->strings->syntax_else){
			if(!expectToken()){
				allocator->deleteObj(if_exp);
				allocator->deleteObj(then_exp);
				return NULL;
			}
			token = recent_token;
			else_exp = expectSingleExpression(scope, true, true);
			/* if(recent_token->type == Tokenizer::BEGIN_CODE_BLOCK){
			else_exp = expectCodeExpression(scope);
			}else{
			else_exp = expectSingleExpression(scope);
			} */
		}else{
			return new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_IF, if_exp->token, if_exp, then_exp OS_DBG_FILEPOS);
		}
		if(!else_exp){
			allocator->deleteObj(if_exp);
			allocator->deleteObj(then_exp);
			return NULL;
		}
		else_exp = expectExpressionValues(else_exp, 0);
		return new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_IF, if_exp->token, if_exp, then_exp, else_exp OS_DBG_FILEPOS);
	}
	return new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_IF, if_exp->token, if_exp, then_exp OS_DBG_FILEPOS);
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectReturnExpression(Scope * scope)
{
	OS_ASSERT(recent_token && recent_token->str == allocator->core->strings->syntax_return);
	Expression * ret_exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_RETURN, recent_token);
	if(!readToken()){
		setError(ERROR_SYNTAX, recent_token);
		allocator->deleteObj(ret_exp);
		return NULL;
	}
	switch(recent_token->type){
	case Tokenizer::END_ARRAY_BLOCK:
	case Tokenizer::END_BRACKET_BLOCK:
	case Tokenizer::END_CODE_BLOCK:
	case Tokenizer::CODE_SEPARATOR:
		return ret_exp;
	}
	Expression * exp = expectSingleExpression(scope, Params().setAllowBinaryOperator(true).setAllowParams(true));
	if(!exp){
		allocator->deleteObj(ret_exp);
		return NULL;
	}
	if(exp->type == EXP_TYPE_PARAMS){
		ret_exp->list.swap(exp->list);
		ret_exp->ret_values = exp->ret_values;
		allocator->deleteObj(exp);
	}else{
		ret_exp->list.add(exp OS_DBG_FILEPOS);
		ret_exp->ret_values = exp->ret_values;
	}
	return ret_exp;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::newBinaryExpression(Scope * scope, ExpressionType exp_type, TokenData * token, Expression * left_exp, Expression * right_exp)
{
	// OS_ASSERT(token->isTypeOf(Tokenizer::BINARY_OPERATOR));
	if(left_exp->isConstValue() && right_exp->isConstValue()){
		struct Lib {
			Compiler * compiler;
			TokenData * token;

			void * malloc(int size OS_DBG_FILEPOS_DECL)
			{
				return compiler->malloc(size OS_DBG_FILEPOS_PARAM);
			}

			Expression * newExpression(const String& str, Expression * left_exp, Expression * right_exp)
			{
				token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(token->text_data, str, Tokenizer::STRING, token->line, token->pos);
				Expression * exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_STRING, token);
				exp->ret_values = 1;
				token->release();
				compiler->allocator->deleteObj(left_exp);
				compiler->allocator->deleteObj(right_exp);
				return exp;
			}

			Expression * newExpression(OS_FLOAT val, Expression * left_exp, Expression * right_exp)
			{
				token = new (malloc(sizeof(TokenData) OS_DBG_FILEPOS)) TokenData(token->text_data, String(compiler->allocator, val), Tokenizer::NUMBER, token->line, token->pos);
				token->setFloat(val);
				Expression * exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_NUMBER, token);
				exp->ret_values = 1;
				token->release();
				compiler->allocator->deleteObj(left_exp);
				compiler->allocator->deleteObj(right_exp);
				return exp;
			}

			Expression * newExpression(OS_INT val, Expression * left_exp, Expression * right_exp)
			{
				return newExpression((OS_FLOAT)val, left_exp, right_exp);
			}

			Expression * switchExpression(bool ret_left, Expression * left_exp, Expression * right_exp)
			{
				if(ret_left){
					compiler->allocator->deleteObj(right_exp);
					return left_exp;
				}
				compiler->allocator->deleteObj(left_exp);
				return right_exp;
			}

		} lib = {this, token};

		switch(exp_type){
		case EXP_TYPE_CONCAT:    // ..
			return lib.newExpression(String(allocator->core->newStringValue(left_exp->toString(), right_exp->toString())), left_exp, right_exp);

			// case EXP_TYPE_ANY_PARAMS:  // ...

			/*
			case EXP_TYPE_LOGIC_AND: // &&
			return lib.switchExpression(left_exp->toInt() == 0, left_exp, right_exp);

			case EXP_TYPE_LOGIC_OR:  // ||
			return lib.switchExpression(left_exp->toInt() != 0, left_exp, right_exp);
			*/

			/*
			case EXP_TYPE_LOGIC_PTR_EQ:  // ===
			case EXP_TYPE_LOGIC_PTR_NE:  // !==
			case EXP_TYPE_LOGIC_EQ:  // ==
			case EXP_TYPE_LOGIC_NE:  // !=
			case EXP_TYPE_LOGIC_GE:  // >=
			case EXP_TYPE_LOGIC_LE:  // <=
			case EXP_TYPE_LOGIC_GREATER: // >
			case EXP_TYPE_LOGIC_LESS:    // <
			*/
			// case EXP_TYPE_LOGIC_NOT:     // !

			// case EXP_TYPE_INC:     // ++
			// case EXP_TYPE_DEC:     // --

			// case EXP_TYPE_QUESTION:  // ?
			// case EXP_TYPE_COLON:     // :

		case EXP_TYPE_BIT_AND: // &
			return lib.newExpression(left_exp->toInt() & right_exp->toInt(), left_exp, right_exp);

		case EXP_TYPE_BIT_OR:  // |
			return lib.newExpression(left_exp->toInt() | right_exp->toInt(), left_exp, right_exp);

		case EXP_TYPE_BIT_XOR: // ^
			return lib.newExpression(left_exp->toInt() ^ right_exp->toInt(), left_exp, right_exp);

			// case EXP_TYPE_BIT_NOT: // ~
		case EXP_TYPE_ADD: // +
			return lib.newExpression(left_exp->toNumber() + right_exp->toNumber(), left_exp, right_exp);

		case EXP_TYPE_SUB: // -
			return lib.newExpression(left_exp->toNumber() - right_exp->toNumber(), left_exp, right_exp);

		case EXP_TYPE_MUL: // *
			return lib.newExpression(left_exp->toNumber() * right_exp->toNumber(), left_exp, right_exp);

		case EXP_TYPE_DIV: // /
			return lib.newExpression(left_exp->toNumber() / right_exp->toNumber(), left_exp, right_exp);

		case EXP_TYPE_MOD: // %
			return lib.newExpression(OS_MATH_MOD_OPERATOR(left_exp->toNumber(), right_exp->toNumber()), left_exp, right_exp);

		case EXP_TYPE_LSHIFT: // <<
			return lib.newExpression(left_exp->toInt() << right_exp->toInt(), left_exp, right_exp);

		case EXP_TYPE_RSHIFT: // >>
			return lib.newExpression(left_exp->toInt() >> right_exp->toInt(), left_exp, right_exp);

		case EXP_TYPE_POW: // **
			return lib.newExpression(OS_MATH_POW_OPERATOR(left_exp->toNumber(), right_exp->toNumber()), left_exp, right_exp);
		}
	}
	switch(exp_type){
	case EXP_TYPE_QUESTION:
		return finishQuestionOperator(scope, token, left_exp, right_exp);

	case EXP_TYPE_ASSIGN:
		{
			if(left_exp->type != EXP_TYPE_PARAMS){
				right_exp = expectExpressionValues(right_exp, 1);
				return newAssingExpression(scope, left_exp, right_exp);
			}
			Expression * values_exp = expectExpressionValues(right_exp, left_exp->list.count);
			for(int i = left_exp->list.count-1; i >= 0; i--){
				OS_ASSERT(values_exp->ret_values > 0);

				Expression * var_exp = left_exp->list[i];
				left_exp->list.removeIndex(i); // left_exp is going to be deleted

				values_exp = newAssingExpression(scope, var_exp, values_exp);
				if(!values_exp){
					break;
				}
			}
			allocator->deleteObj(left_exp);
			return values_exp;
		}
	}
	if(left_exp->type == EXP_TYPE_PARAMS){
		OS_ASSERT(right_exp->type != EXP_TYPE_PARAMS);
		right_exp = expectExpressionValues(right_exp, 1);
		left_exp->list.add(right_exp OS_DBG_FILEPOS);
		left_exp->ret_values++;
		return left_exp;
	}
	if(right_exp->type == EXP_TYPE_PARAMS){
		Expression * params = right_exp;
		OS_ASSERT(params->list.count > 0);
		if(params->list.count == 1){
			right_exp = params->list[0];
			allocator->vectorClear(params->list);
			allocator->deleteObj(params);
		}else{
			left_exp = expectExpressionValues(left_exp, 1);
			allocator->vectorInsertAtIndex(params->list, 0, left_exp OS_DBG_FILEPOS);
			params->ret_values++;
			return params; 
		}
	}
	left_exp = expectExpressionValues(left_exp, 1);
	right_exp = expectExpressionValues(right_exp, 1);
	Expression * exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(exp_type, token, left_exp, right_exp OS_DBG_FILEPOS);
	exp->ret_values = exp_type == EXP_TYPE_PARAMS ? 2 : 1;
	return exp;
}

bool OS::Core::Compiler::findLocalVar(LocalVarDesc& desc, Scope * scope, const String& name, int active_locals, bool all_scopes)
{
	OS_ASSERT(scope);
	for(int up_count = 0, up_scope_count = 0;;){
		for(int i = scope->locals.count-1; i >= 0; i--){
			const Scope::LocalVar& local_var = scope->locals[i];
			if((up_count || local_var.index < active_locals) && local_var.name == name){
				desc.index = local_var.index;
				desc.up_count = up_count;
				desc.up_scope_count = up_scope_count;
				desc.type = i < scope->num_params ? LOCAL_PARAM : (name.toChar()[0] != OS_TEXT('#') ? LOCAL_GENERIC : LOCAL_TEMP);
				return true;
			}
		}
		if(scope->parent){
			if(!all_scopes){
				return false;
			}
			if(scope->type == EXP_TYPE_FUNCTION){
				up_count++;
			}
			up_scope_count++;
			scope = scope->parent;
			continue;
		}
		break;
	}
	return false;
}

OS::Core::Compiler::Expression * OS::Core::Compiler::newAssingExpression(Scope * scope, Expression * var_exp, Expression * value_exp)
{
	switch(var_exp->type){
	case EXP_TYPE_CALL_DIM:
		{
			Expression * name_exp = var_exp->list[0];
			Expression * params = var_exp->list[1];
			OS_ASSERT(params->type == EXP_TYPE_PARAMS);
			allocator->vectorInsertAtIndex(var_exp->list, 0, value_exp OS_DBG_FILEPOS);
			var_exp->type = EXP_TYPE_SET_DIM;
			var_exp->ret_values = value_exp->ret_values-1;
			return var_exp;
		}

	case EXP_TYPE_INDIRECT:
		{
			OS_ASSERT(var_exp->list.count == 2);
			Expression * var_exp_left = var_exp->list[0];
			switch(var_exp_left->type){
			case EXP_TYPE_NAME:
				{
					OS_ASSERT(var_exp_left->ret_values == 1);
					if(findLocalVar(var_exp_left->local_var, scope, var_exp_left->token->str, var_exp_left->active_locals, true)){
						var_exp_left->type = EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE;
						if(scope->function->max_up_count < var_exp_left->local_var.up_count){
							scope->function->max_up_count = var_exp_left->local_var.up_count;
						}
					}else{
						var_exp_left->type = EXP_TYPE_GET_ENV_VAR_AUTO_CREATE;
					}
					break;
				}
			}
			ExpressionType exp_type = EXP_TYPE_SET_PROPERTY;
			Expression * var_exp_right = var_exp->list[1];
			switch(var_exp_right->type){
			case EXP_TYPE_NAME:
				OS_ASSERT(var_exp_right->ret_values == 1);
				var_exp_right->type = EXP_TYPE_CONST_STRING;
				break;

			case EXP_TYPE_CALL:
			case EXP_TYPE_CALL_AUTO_PARAM:
				OS_ASSERT(false);
				return NULL;

			case EXP_TYPE_CALL_DIM:
				OS_ASSERT(false);
				return NULL;
			}
			Expression * exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(exp_type, var_exp->token, value_exp, var_exp_left, var_exp_right OS_DBG_FILEPOS);
			exp->ret_values = value_exp->ret_values-1;
			allocator->vectorClear(var_exp->list);
			allocator->deleteObj(var_exp);
			return exp;
		}
		break;

	case EXP_TYPE_NAME:
		if(findLocalVar(var_exp->local_var, scope, var_exp->token->str, var_exp->active_locals, true)){
			var_exp->type = EXP_TYPE_SET_LOCAL_VAR;
			if(scope->function->max_up_count < var_exp->local_var.up_count){
				scope->function->max_up_count = var_exp->local_var.up_count;
			}		
		}else{
			var_exp->type = EXP_TYPE_SET_ENV_VAR;
		}
		var_exp->list.add(value_exp OS_DBG_FILEPOS);
		var_exp->ret_values = value_exp->ret_values-1;
		return var_exp;

	default:
		// OS_ASSERT(false);
		if(!var_exp->isWriteable()){
			setError(ERROR_EXPECT_WRITEABLE, var_exp->token);
			allocator->deleteObj(var_exp);
			allocator->deleteObj(value_exp);
			return NULL;
		}
	}
	return new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_ASSIGN, var_exp->token, var_exp, value_exp OS_DBG_FILEPOS);
}

OS::Core::Compiler::Expression * OS::Core::Compiler::finishBinaryOperator(Scope * scope, OpcodeLevel prev_level, Expression * exp, 
	const Params& _p, bool& is_finished)
{
	TokenData * binary_operator = recent_token;
	OS_ASSERT(binary_operator->isTypeOf(Tokenizer::BINARY_OPERATOR));

	Params p = Params(_p)
		.setAllowAssign(false)
		.setAllowBinaryOperator(false)
		.setAllowInOperator(_p.allow_in_operator)
		// .setAllowParams(false)
		.setAllowAutoCall(false) // binary_operator->type == Tokenizer::OPERATOR_ASSIGN)
		.setAllowRootBlocks(false);

	readToken();
	Expression * exp2 = expectSingleExpression(scope, Params(p).setAllowParams(false)); // false, allow_param, false, false, false);
	if(!exp2){
		/* if(!isError()){
		return exp;
		} */
		is_finished = true;
		allocator->deleteObj(exp);
		return NULL;
	}
	// exp2 = expectExpressionValues(exp2, 1);
	if(recent_token && recent_token->type == Tokenizer::NAME){
		if(recent_token->str == allocator->core->strings->syntax_in){
			if(p.allow_in_operator){
				recent_token->type = Tokenizer::OPERATOR_IN;
			}
		}else if(recent_token->str == allocator->core->strings->syntax_isprototypeof){
			recent_token->type = Tokenizer::OPERATOR_ISPROTOTYPEOF;
		}else if(recent_token->str == allocator->core->strings->syntax_is){
			recent_token->type = Tokenizer::OPERATOR_IS;
		}
	}
	if(!recent_token || !recent_token->isTypeOf(Tokenizer::BINARY_OPERATOR) || (!p.allow_params && recent_token->type == Tokenizer::PARAM_SEPARATOR)){
		// return new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(getExpressionType(binary_operator->type), binary_operator, exp, exp2);
		is_finished = true;
		return newBinaryExpression(scope, getExpressionType(binary_operator->type), binary_operator, exp, exp2);
	}
	ExpressionType left_exp_type = getExpressionType(binary_operator->type);
	ExpressionType right_exp_type = getExpressionType(recent_token->type);
	OpcodeLevel left_level = getOpcodeLevel(left_exp_type);
	OpcodeLevel right_level = getOpcodeLevel(right_exp_type);
	if(left_level == right_level){
		// exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(left_exp_type, binary_operator, exp, exp2);
		exp = newBinaryExpression(scope, left_exp_type, binary_operator, exp, exp2);
		return finishBinaryOperator(scope, prev_level, exp, p, is_finished);
	}
	if(left_level > right_level){
		// exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(left_exp_type, binary_operator, exp, exp2);
		exp = newBinaryExpression(scope, left_exp_type, binary_operator, exp, exp2);
		if(prev_level >= right_level){
			is_finished = false;
			return exp;
		}
		return finishBinaryOperator(scope, prev_level, exp, p, is_finished);
	}
	exp2 = finishBinaryOperator(scope, left_level, exp2, p, is_finished);
	if(!exp2){
		allocator->deleteObj(exp);
		return NULL;
	}
	exp = newBinaryExpression(scope, left_exp_type, binary_operator, exp, exp2);
	if(is_finished){ // !recent_token || !recent_token->isTypeOf(Tokenizer::BINARY_OPERATOR)){
		return exp;
	}
	return finishBinaryOperator(scope, prev_level, exp, p, is_finished);
}

OS::Core::Compiler::Expression * OS::Core::Compiler::finishValueExpressionNoAutoCall(Scope * scope, Expression * exp, const Params& p)
{
	return finishValueExpression(scope, exp, Params(p).setAllowAutoCall(false));
}

OS::Core::Compiler::Expression * OS::Core::Compiler::finishValueExpressionNoNextCall(Scope * scope, Expression * exp, const Params& p)
{
	if(recent_token && recent_token->type == Tokenizer::BEGIN_BRACKET_BLOCK){
		return exp;
	}
	return finishValueExpression(scope, exp, Params(p).setAllowAutoCall(false));
}

OS::Core::Compiler::Expression * OS::Core::Compiler::finishValueExpression(Scope * scope, Expression * exp, const Params& _p)
{
	bool is_finished;
	Params p = Params(_p)
		.setAllowRootBlocks(false);
	bool next_allow_auto_call = false;
	for(;; p.allow_auto_call = next_allow_auto_call, next_allow_auto_call = false){
		if(!recent_token){
			return exp;
		}
		Expression * exp2;
		TokenData * token = recent_token;
		TokenType token_type = token->type;
		switch(token_type){
		case Tokenizer::OPERATOR_INDIRECT:    // .
			// setError(ERROR_SYNTAX, token);
			// return NULL;
			token = expectToken(Tokenizer::NAME);
			if(!token){
				allocator->deleteObj(exp);
				return NULL;
			}
			exp2 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, token);
			exp2->ret_values = 1;
			OS_ASSERT(scope->function);
			exp2->active_locals = scope->function->num_locals;
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_INDIRECT, exp2->token, exp, exp2 OS_DBG_FILEPOS);
			exp->ret_values = 1;
			readToken();
			// p.setAllowCall(true);
			next_allow_auto_call = p.allow_auto_call;
			continue;

			// post ++, post --
		case Tokenizer::OPERATOR_INC:
		case Tokenizer::OPERATOR_DEC:
			if(exp->type != EXP_TYPE_NAME){
				return exp;
			}
			OS_ASSERT(exp->ret_values == 1);
			if(!findLocalVar(exp->local_var, scope, exp->token->str, exp->active_locals, true)){
				setError(ERROR_LOCAL_VAL_NOT_DECLARED, exp->token);
				allocator->deleteObj(exp);
				return NULL;
			}
			if(scope->function->max_up_count < exp->local_var.up_count){
				scope->function->max_up_count = exp->local_var.up_count;
			}
			exp->type = EXP_TYPE_GET_LOCAL_VAR;
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(token_type == Tokenizer::OPERATOR_INC ? EXP_TYPE_POST_INC : EXP_TYPE_POST_DEC, exp->token, exp OS_DBG_FILEPOS);
			exp->ret_values = 1;
			readToken();
			return finishValueExpressionNoAutoCall(scope, exp, p);

		case Tokenizer::OPERATOR_CONCAT:    // ..
			// case Tokenizer::REST_ARGUMENTS:  // ...

		case Tokenizer::OPERATOR_LOGIC_AND: // &&
		case Tokenizer::OPERATOR_LOGIC_OR:  // ||

		case Tokenizer::OPERATOR_LOGIC_PTR_EQ:  // ===
		case Tokenizer::OPERATOR_LOGIC_PTR_NE:  // !==
		case Tokenizer::OPERATOR_LOGIC_EQ:  // ==
		case Tokenizer::OPERATOR_LOGIC_NE:  // !=
		case Tokenizer::OPERATOR_LOGIC_GE:  // >=
		case Tokenizer::OPERATOR_LOGIC_LE:  // <=
		case Tokenizer::OPERATOR_LOGIC_GREATER: // >
		case Tokenizer::OPERATOR_LOGIC_LESS:    // <
		case Tokenizer::OPERATOR_LOGIC_NOT:     // !

			// case Tokenizer::OPERATOR_INC:     // ++
			// case Tokenizer::OPERATOR_DEC:     // --

		case Tokenizer::OPERATOR_QUESTION:  // ?
			// case Tokenizer::OPERATOR_COLON:     // :

		case Tokenizer::OPERATOR_BIT_AND: // &
		case Tokenizer::OPERATOR_BIT_OR:  // |
		case Tokenizer::OPERATOR_BIT_XOR: // ^
		case Tokenizer::OPERATOR_BIT_NOT: // ~
		case Tokenizer::OPERATOR_ADD: // +
		case Tokenizer::OPERATOR_SUB: // -
		case Tokenizer::OPERATOR_MUL: // *
		case Tokenizer::OPERATOR_DIV: // /
		case Tokenizer::OPERATOR_MOD: // %
		case Tokenizer::OPERATOR_LSHIFT: // <<
		case Tokenizer::OPERATOR_RSHIFT: // >>
		case Tokenizer::OPERATOR_POW: // **
			if(!p.allow_binary_operator){
				return exp;
			}
			exp = finishBinaryOperator(scope, OP_LEVEL_NOTHING, exp, p, is_finished);
			if(!exp){
				return NULL;
			}
			OS_ASSERT(is_finished);
			continue;

		case Tokenizer::PARAM_SEPARATOR:
			if(!p.allow_params){
				return exp;
			}
			exp = finishBinaryOperator(scope, OP_LEVEL_NOTHING, exp, p, is_finished);
			if(!exp){
				return NULL;
			}
			OS_ASSERT(is_finished);
			continue;

		case Tokenizer::OPERATOR_BIT_AND_ASSIGN: // &=
		case Tokenizer::OPERATOR_BIT_OR_ASSIGN:  // |=
		case Tokenizer::OPERATOR_BIT_XOR_ASSIGN: // ^=
		case Tokenizer::OPERATOR_BIT_NOT_ASSIGN: // ~=
		case Tokenizer::OPERATOR_ADD_ASSIGN: // +=
		case Tokenizer::OPERATOR_SUB_ASSIGN: // -=
		case Tokenizer::OPERATOR_MUL_ASSIGN: // *=
		case Tokenizer::OPERATOR_DIV_ASSIGN: // /=
		case Tokenizer::OPERATOR_MOD_ASSIGN: // %=
		case Tokenizer::OPERATOR_LSHIFT_ASSIGN: // <<=
		case Tokenizer::OPERATOR_RSHIFT_ASSIGN: // >>=
		case Tokenizer::OPERATOR_POW_ASSIGN: // **=
			setError(ERROR_SYNTAX, token);
			return NULL;

		case Tokenizer::OPERATOR_ASSIGN: // =
			if(!p.allow_assing){ // allow_binary_operator){
				return exp;
			}
			exp = finishBinaryOperator(scope, OP_LEVEL_NOTHING, exp, Params(p).setAllowAssign(false), is_finished);
			if(!exp){
				return NULL;
			}
			OS_ASSERT(is_finished);
			return exp;

		case Tokenizer::END_ARRAY_BLOCK:
		case Tokenizer::END_BRACKET_BLOCK:
		case Tokenizer::END_CODE_BLOCK:
		case Tokenizer::CODE_SEPARATOR:
			return exp;

		case Tokenizer::BEGIN_CODE_BLOCK: // {
			/* if(!p.allow_auto_call){
			return exp;
			} */
			exp2 = expectObjectOrFunctionExpression(scope, p, false);
			if(!exp2){
				allocator->deleteObj(exp);
				return NULL;
			}
			OS_ASSERT(exp2->ret_values == 1);
			exp2 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, exp2->token, exp2 OS_DBG_FILEPOS);
			exp2->ret_values = 1;
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CALL_AUTO_PARAM, token, exp, exp2 OS_DBG_FILEPOS);
			exp->ret_values = 1;
			// allow_auto_call = false;
			continue;

		case Tokenizer::NAME:
			if(token->str == allocator->core->strings->syntax_in){
				if(!p.allow_in_operator || !p.allow_binary_operator){
					return exp;
				}
				token->type = Tokenizer::OPERATOR_IN;
				exp = finishBinaryOperator(scope, OP_LEVEL_NOTHING, exp, p, is_finished);
				if(!exp){
					return NULL;
				}
				OS_ASSERT(is_finished);
				continue;
			}
			if(token->str == allocator->core->strings->syntax_isprototypeof){
				if(!p.allow_binary_operator){
					return exp;
				}
				token->type = Tokenizer::OPERATOR_ISPROTOTYPEOF;
				exp = finishBinaryOperator(scope, OP_LEVEL_NOTHING, exp, p, is_finished);
				if(!exp){
					return NULL;
				}
				OS_ASSERT(is_finished);
				continue;
			}
			if(token->str == allocator->core->strings->syntax_is){
				if(!p.allow_binary_operator){
					return exp;
				}
				token->type = Tokenizer::OPERATOR_IS;
				exp = finishBinaryOperator(scope, OP_LEVEL_NOTHING, exp, p, is_finished);
				if(!exp){
					return NULL;
				}
				OS_ASSERT(is_finished);
				continue;
			}
			// no break

		default:
			if(!p.allow_auto_call){
				return exp;
			}
			if(token->type == Tokenizer::NAME){
				Core::Strings * strings = allocator->core->strings;
				if(token->str == strings->syntax_var
					// || token->str == strings->syntax_function
					|| token->str == strings->syntax_return
					|| token->str == strings->syntax_if
					|| token->str == strings->syntax_else
					|| token->str == strings->syntax_elseif
					|| token->str == strings->syntax_for
					|| token->str == strings->syntax_break
					|| token->str == strings->syntax_continue
					|| token->str == strings->syntax_in
					|| token->str == strings->syntax_class
					|| token->str == strings->syntax_enum
					|| token->str == strings->syntax_switch
					|| token->str == strings->syntax_case 
					|| token->str == strings->syntax_default
					|| token->str == strings->syntax_try
					|| token->str == strings->syntax_catch
					|| token->str == strings->syntax_finally
					|| token->str == strings->syntax_throw
					|| token->str == strings->syntax_new
					|| token->str == strings->syntax_while
					|| token->str == strings->syntax_do
					|| token->str == strings->syntax_const
					|| token->str == strings->syntax_public
					|| token->str == strings->syntax_protected
					|| token->str == strings->syntax_private
					|| token->str == strings->syntax_yield
					|| token->str == strings->syntax_static
					|| token->str == strings->syntax_debugger
					// || token->str == strings->syntax_debuglocals
					)
				{
					return exp;
				}
			}
			exp2 = expectSingleExpression(scope, Params(p)
				.setAllowAssign(false)
				.setAllowAutoCall(false)
				.setAllowParams(false)
				.setAllowRootBlocks(false)); // allow_binary_operator, false, false, false, false);
			if(!exp2){
				allocator->deleteObj(exp);
				return NULL;
			}
			OS_ASSERT(exp2->ret_values == 1);
			exp2 = expectExpressionValues(exp2, 1);
			exp2 = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_PARAMS, exp2->token, exp2 OS_DBG_FILEPOS);
			exp2->ret_values = 1;
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CALL_AUTO_PARAM, token, exp, exp2 OS_DBG_FILEPOS);
			exp->ret_values = 1;
			// allow_auto_call = false;
			continue;

		case Tokenizer::BEGIN_BRACKET_BLOCK: // (
			if(!p.allow_call){
				return exp;
			}
			exp2 = expectParamsExpression(scope);
			if(!exp2){
				allocator->deleteObj(exp);
				return NULL;
			}
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CALL, token, exp, exp2 OS_DBG_FILEPOS);
			exp->ret_values = 1;
			continue;

		case Tokenizer::BEGIN_ARRAY_BLOCK: // [
			exp2 = expectParamsExpression(scope);
			if(!exp2){
				allocator->deleteObj(exp);
				return NULL;
			}
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CALL_DIM, token, exp, exp2 OS_DBG_FILEPOS);
			exp->ret_values = 1;
			if(0 && !p.allow_binary_operator){
				return exp;
			}
			continue;
		}
	}
	return NULL; // shut up compiler
}

OS::Core::Compiler::Params::Params()
{
	OS_MEMSET(this, 0, sizeof(*this));
	allow_call = true;
}

OS::Core::Compiler::Params::Params(const Params& p)
{
	OS_MEMCPY(this, &p, sizeof(p));
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowRootBlocks(bool val)
{
	allow_root_blocks = val;
	allow_var_decl = val;
	return *this;
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowVarDecl(bool val)
{
	allow_var_decl = val;
	return *this;
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowInlineNestedBlock(bool val)
{
	allow_inline_nested_block = val;
	return *this;
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowBinaryOperator(bool val)
{
	allow_binary_operator = val;
	allow_in_operator = val;
	return *this;
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowInOperator(bool val)
{
	allow_in_operator = val;
	return *this;
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowAssign(bool val)
{
	allow_assing = val;
	return *this;
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowParams(bool val)
{
	allow_params = val;
	return *this;
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowAutoCall(bool val)
{
	allow_auto_call = val;
	return *this;
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowCall(bool val)
{
	allow_call = val;
	return *this;
}

OS::Core::Compiler::Params& OS::Core::Compiler::Params::setAllowNopResult(bool val)
{
	allow_nop_result = val;
	return *this;
}

bool OS::Core::Compiler::isVarNameValid(const String& name)
{
	Core::Strings * strings = allocator->core->strings;
	return !(name == strings->syntax_super
		|| name == strings->syntax_is
		|| name == strings->syntax_isprototypeof
		/*
		|| name == strings->syntax_typeof
		|| name == strings->syntax_valueof
		|| name == strings->syntax_booleanof
		|| name == strings->syntax_numberof
		|| name == strings->syntax_stringof
		|| name == strings->syntax_arrayof
		|| name == strings->syntax_objectof
		|| name == strings->syntax_userdataof
		|| name == strings->syntax_functionof
		|| name == strings->syntax_clone
		*/
		|| name == strings->syntax_extends
		|| name == strings->syntax_delete
		|| name == strings->syntax_prototype
		|| name == strings->syntax_var
		|| name == strings->syntax_this
		|| name == strings->syntax_arguments
		|| name == strings->syntax_function
		|| name == strings->syntax_null
		|| name == strings->syntax_true
		|| name == strings->syntax_false
		|| name == strings->syntax_return
		|| name == strings->syntax_class
		|| name == strings->syntax_enum
		|| name == strings->syntax_switch
		|| name == strings->syntax_case
		|| name == strings->syntax_default
		|| name == strings->syntax_if
		|| name == strings->syntax_else
		|| name == strings->syntax_elseif
		|| name == strings->syntax_for
		|| name == strings->syntax_in
		|| name == strings->syntax_break
		|| name == strings->syntax_continue
		|| name == strings->syntax_try
		|| name == strings->syntax_catch
		|| name == strings->syntax_finally
		|| name == strings->syntax_throw
		|| name == strings->syntax_new
		|| name == strings->syntax_while
		|| name == strings->syntax_do
		|| name == strings->syntax_const
		|| name == strings->syntax_public
		|| name == strings->syntax_protected
		|| name == strings->syntax_private
		|| name == strings->syntax_yield
		|| name == strings->syntax_static
		|| name == strings->syntax_debugger
		|| name == strings->syntax_debuglocals
		|| name == strings->var_env
#ifdef OS_GLOBAL_VAR_ENABLED
		|| name == strings->var_globals
#endif
		);
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectSingleExpression(Scope * scope, bool allow_nop_result, bool allow_inline_nested_block)
{
	return expectSingleExpression(scope, Params()
		.setAllowAssign(true)
		.setAllowAutoCall(true)
		.setAllowBinaryOperator(true)
		.setAllowParams(true)
		.setAllowRootBlocks(true)
		.setAllowNopResult(allow_nop_result)
		.setAllowInlineNestedBlock(allow_inline_nested_block));
}

OS::Core::Compiler::Expression * OS::Core::Compiler::expectSingleExpression(Scope * scope, const Params& p)
{
#ifdef OS_DEBUG
	allocator->checkNativeStackUsage(OS_TEXT("OS::Core::Compiler::expectSingleExpression"));
#endif
	TokenData * token = recent_token; // readToken();
	if(!token){
		setError(ERROR_EXPECT_EXPRESSION, token);
		return NULL;
	}
	Expression * exp;
	TokenType token_type = token->type;
	switch(token_type){
		// begin unary operators
	case Tokenizer::OPERATOR_ADD:
	case Tokenizer::OPERATOR_SUB:
	case Tokenizer::OPERATOR_LENGTH:
	case Tokenizer::OPERATOR_BIT_NOT:
	case Tokenizer::OPERATOR_LOGIC_NOT:
		if(!expectToken()){
			return NULL;
		}
		exp = expectSingleExpression(scope, Params());
		if(!exp){
			return NULL;
		}
		OS_ASSERT(exp->ret_values == 1);
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(getUnaryExpressionType(token_type), exp->token, exp OS_DBG_FILEPOS);
		exp->ret_values = 1;
		return finishValueExpressionNoAutoCall(scope, exp, p); // allow_binary_operator, allow_param, allow_assign, false);

		// pre ++, pre --
	case Tokenizer::OPERATOR_INC:
	case Tokenizer::OPERATOR_DEC:
		if(!expectToken(Tokenizer::NAME)){
			return NULL;
		}
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_LOCAL_VAR, recent_token);
		exp->ret_values = 1;
		exp->active_locals = scope->function->num_locals;
		if(!findLocalVar(exp->local_var, scope, exp->token->str, exp->active_locals, true)){
			setError(ERROR_LOCAL_VAL_NOT_DECLARED, exp->token);
			allocator->deleteObj(exp);
			return NULL;
		}
		if(scope->function->max_up_count < exp->local_var.up_count){
			scope->function->max_up_count = exp->local_var.up_count;
		}
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(token_type == Tokenizer::OPERATOR_INC ? EXP_TYPE_PRE_INC : EXP_TYPE_PRE_DEC, exp->token, exp OS_DBG_FILEPOS);
		exp->ret_values = 1;
		readToken();
		return finishValueExpressionNoAutoCall(scope, exp, p);
		// end unary operators

	case Tokenizer::BEGIN_CODE_BLOCK:
		if(p.allow_root_blocks){
			if(!p.allow_inline_nested_block){
				TokenData * check_token = getPrevToken();
				if(!check_token || (check_token->type != Tokenizer::CODE_SEPARATOR && check_token->type != Tokenizer::BEGIN_CODE_BLOCK)){
					// setError(Tokenizer::CODE_SEPARATOR, recent_token);
					setError(ERROR_EXPECT_CODE_SEP_BEFORE_NESTED_BLOCK, recent_token);
					return NULL;
				}
			}
			return expectCodeExpression(scope);
		}
		return expectObjectOrFunctionExpression(scope, p);

	case Tokenizer::BEGIN_ARRAY_BLOCK:
		return expectArrayExpression(scope, p);

	case Tokenizer::BEGIN_BRACKET_BLOCK:
		return expectBracketExpression(scope, p);

	case Tokenizer::STRING:
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_STRING, token);
		exp->ret_values = 1;
		readToken();
		return finishValueExpressionNoNextCall(scope, exp, p);

	case Tokenizer::NUMBER:
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_NUMBER, token);
		exp->ret_values = 1;
		readToken();
		return finishValueExpressionNoNextCall(scope, exp, p);

	case Tokenizer::REST_ARGUMENTS:
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_REST_ARGUMENTS, token);
		exp->ret_values = 1;
		readToken();
		return finishValueExpressionNoNextCall(scope, exp, p);

	case Tokenizer::CODE_SEPARATOR:
	case Tokenizer::END_ARRAY_BLOCK:
	case Tokenizer::END_BRACKET_BLOCK:
	case Tokenizer::END_CODE_BLOCK:
		if(!p.allow_nop_result){
			return NULL;
		}
		return new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NOP, token);

	case Tokenizer::NAME:
		if(token->str == allocator->core->strings->syntax_var){
			if(!p.allow_var_decl){
				setError(ERROR_NESTED_ROOT_BLOCK, token);
				return NULL;
			}
			return expectVarExpression(scope);
		}
		if(token->str == allocator->core->strings->syntax_function){
			exp = expectFunctionExpression(scope);
			if(!exp){
				return NULL;
			}
			if(!exp->ret_values){
				return exp;
			}
			return finishValueExpression(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_return){
			return expectReturnExpression(scope);
		}
		if(token->str == allocator->core->strings->syntax_if){
			if(!p.allow_root_blocks){
				setError(ERROR_NESTED_ROOT_BLOCK, token);
				return NULL;
			}
			return expectIfExpression(scope);
		}
		if(token->str == allocator->core->strings->syntax_else){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_elseif){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_for){
			if(!p.allow_root_blocks){
				setError(ERROR_NESTED_ROOT_BLOCK, token);
				return NULL;
			}
			return expectForExpression(scope);
		}
		if(token->str == allocator->core->strings->syntax_in){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_this){
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_THIS, token);
			exp->ret_values = 1;
			readToken();
			return finishValueExpression(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_arguments){
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_GET_ARGUMENTS, token);
			exp->ret_values = 1;
			readToken();
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_null){
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_NULL, token);
			exp->ret_values = 1;
			readToken();
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_true){
			token->setFloat(1);
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_TRUE, token);
			exp->ret_values = 1;
			readToken();
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_false){
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONST_FALSE, token);
			exp->ret_values = 1;
			readToken();
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_extends){
			exp = expectExtendsExpression(scope);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		/*
		if(token->str == allocator->core->strings->syntax_clone){
			exp = expectCloneExpression(scope);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		*/
		if(token->str == allocator->core->strings->syntax_delete){
			if(!p.allow_root_blocks){
				setError(ERROR_NESTED_ROOT_BLOCK, token);
				return NULL;
			}
			return expectDeleteExpression(scope);
		}
		/*
		if(token->str == allocator->core->strings->syntax_typeof){
			exp = expectValueOfExpression(scope, EXP_TYPE_TYPE_OF);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_valueof){
			exp = expectValueOfExpression(scope, EXP_TYPE_VALUE_OF);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_booleanof){
			exp = expectValueOfExpression(scope, EXP_TYPE_LOGIC_BOOL);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_numberof){
			exp = expectValueOfExpression(scope, EXP_TYPE_NUMBER_OF);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_stringof){
			exp = expectValueOfExpression(scope, EXP_TYPE_STRING_OF);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_arrayof){
			exp = expectValueOfExpression(scope, EXP_TYPE_ARRAY_OF);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_objectof){
			exp = expectValueOfExpression(scope, EXP_TYPE_OBJECT_OF);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_userdataof){
			exp = expectValueOfExpression(scope, EXP_TYPE_USERDATA_OF);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_functionof){
			exp = expectValueOfExpression(scope, EXP_TYPE_FUNCTION_OF);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		*/
		if(token->str == allocator->core->strings->syntax_break){
			if(!p.allow_root_blocks){
				setError(ERROR_NESTED_ROOT_BLOCK, token);
				return NULL;
			}
			readToken();
			return new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_BREAK, token);
		}
		if(token->str == allocator->core->strings->syntax_continue){
			if(!p.allow_root_blocks){
				setError(ERROR_NESTED_ROOT_BLOCK, token);
				return NULL;
			}
			readToken();
			return new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_CONTINUE, token);
		}
		if(token->str == allocator->core->strings->syntax_debugger){
			if(!p.allow_root_blocks){
				setError(ERROR_NESTED_ROOT_BLOCK, token);
				return NULL;
			}
			readToken();
			return new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_DEBUGGER, token);
		}
		if(token->str == allocator->core->strings->syntax_debuglocals){
			exp = expectDebugLocalsExpression(scope);
			if(!exp){
				return NULL;
			}
			return finishValueExpressionNoAutoCall(scope, exp, p);
		}
		if(token->str == allocator->core->strings->syntax_is){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_isprototypeof){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_class){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_enum){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_switch){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_case || token->str == allocator->core->strings->syntax_default){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_try){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_catch){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_finally){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_throw){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_new){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_while){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_do){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_const){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_public){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_protected){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_private){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_yield){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_static){
			setError(ERROR_SYNTAX, token);
			return NULL;
		}
		if(token->str == allocator->core->strings->syntax_super){
			exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_SUPER, token);
			exp->ret_values = 1;
			readToken();
			return finishValueExpression(scope, exp, p);
		}
		exp = new (malloc(sizeof(Expression) OS_DBG_FILEPOS)) Expression(EXP_TYPE_NAME, token);
		exp->ret_values = 1;
		OS_ASSERT(scope->function);
		exp->active_locals = scope->function->num_locals;
		readToken();
		return finishValueExpression(scope, exp, p);
	}
	setError(ERROR_EXPECT_EXPRESSION, token);
	return NULL;
}

void OS::Core::Compiler::debugPrintSourceLine(StringBuffer& out, TokenData * token)
{
	if(!token){
		return;
	}

	bool filePrinted = false;
	if(recent_printed_text_data != token->text_data){
		if(recent_printed_text_data){
			recent_printed_text_data->release();
		}
		filePrinted = true;
		recent_printed_line = -1;
		recent_printed_text_data = token->text_data->retain();
		out += String::format(allocator, OS_TEXT("\n[FILE] %s"), token->text_data->filename.toChar());
	}
	if(recent_printed_line != token->line && token->line >= 0){
		recent_printed_line = token->line;
		String line = allocator->core->newStringValue(token->text_data->lines[token->line], true, true);
		out += String::format(allocator, OS_TEXT("\n[%d] %s\n\n"), token->line+1, line.toChar());
	}
	else if(filePrinted){
		out += String::format(allocator, OS_TEXT("\n"));
	}
	return;
}

const OS_CHAR * OS::Core::Compiler::getExpName(ExpressionType type)
{
	switch(type){
	case EXP_TYPE_NOP:
		return OS_TEXT("nop");

	case EXP_TYPE_CODE_LIST:
		return OS_TEXT("code list");

	case EXP_TYPE_CONST_NUMBER:
		return OS_TEXT("const number");

	case EXP_TYPE_CONST_STRING:
		return OS_TEXT("const string");

	case EXP_TYPE_CONST_NULL:
		return OS_TEXT("const null");

	case EXP_TYPE_CONST_TRUE:
		return OS_TEXT("const true");

	case EXP_TYPE_CONST_FALSE:
		return OS_TEXT("const false");

	case EXP_TYPE_NAME:
		return OS_TEXT("name");

	case EXP_TYPE_PARAMS:
		return OS_TEXT("params");

	case EXP_TYPE_INDIRECT:
		return OS_TEXT("indirect");

	case EXP_TYPE_SET_PROPERTY:
		return OS_TEXT("set property");

	case EXP_TYPE_SET_PROPERTY_BY_LOCALS_AUTO_CREATE:
		return OS_TEXT("set property by locals auto create");

	case EXP_TYPE_GET_SET_PROPERTY_BY_LOCALS_AUTO_CREATE:
		return OS_TEXT("get & set property by locals auto create");

	case EXP_TYPE_GET_PROPERTY:
		return OS_TEXT("get property");

	case EXP_TYPE_GET_THIS_PROPERTY_BY_STRING:
		return OS_TEXT("get this property by string");

	case EXP_TYPE_GET_PROPERTY_BY_LOCALS:
		return OS_TEXT("get property by locals");

	case EXP_TYPE_GET_PROPERTY_BY_LOCAL_AND_NUMBER:
		return OS_TEXT("get property by local & number");

	case EXP_TYPE_GET_PROPERTY_AUTO_CREATE:
		return OS_TEXT("get property auto create");

	case EXP_TYPE_SET_DIM:
		return OS_TEXT("set dim");

	case EXP_TYPE_POP_VALUE:
		return OS_TEXT("pop");

	case EXP_TYPE_CALL:
	case EXP_TYPE_CALL_AUTO_PARAM:
		return OS_TEXT("call");

	case EXP_TYPE_SUPER_CALL:
		return OS_TEXT("super call");

	case EXP_TYPE_CALL_DIM:
		return OS_TEXT("dim");

		// case EXP_TYPE_GET_DIM:
		//	return OS_TEXT("get dim");

	case EXP_TYPE_CALL_METHOD:
		return OS_TEXT("call method");

	case EXP_TYPE_TAIL_CALL_METHOD:
		return OS_TEXT("tail call method");

	case EXP_TYPE_TAIL_CALL:
		return OS_TEXT("tail call");

	case EXP_TYPE_VALUE:
		return OS_TEXT("single value");

	case EXP_TYPE_EXTENDS:
		return OS_TEXT("extends");

	/*
	case EXP_TYPE_CLONE:
		return OS_TEXT("clone");
	*/

	case EXP_TYPE_DELETE:
		return OS_TEXT("delete");

	case EXP_TYPE_RETURN:
		return OS_TEXT("return");

	case EXP_TYPE_FUNCTION:
		return OS_TEXT("function");

	case EXP_TYPE_SCOPE:
		return OS_TEXT("scope");

	case EXP_TYPE_LOOP_SCOPE:
		return OS_TEXT("loop");

	case EXP_TYPE_GET_THIS:
		return OS_TEXT("push this");

	case EXP_TYPE_GET_ARGUMENTS:
		return OS_TEXT("push arguments");

	case EXP_TYPE_GET_REST_ARGUMENTS:
		return OS_TEXT("push rest arguments");

	case EXP_TYPE_GET_LOCAL_VAR:
		return OS_TEXT("get local var");

	case EXP_TYPE_GET_LOCAL_VAR_AUTO_CREATE:
		return OS_TEXT("get local var auto create");

	case EXP_TYPE_GET_ENV_VAR:
		return OS_TEXT("get env var");

	case EXP_TYPE_GET_ENV_VAR_AUTO_CREATE:
		return OS_TEXT("get env var auto create");

	case EXP_TYPE_SET_LOCAL_VAR:
		return OS_TEXT("set local var");

	case EXP_TYPE_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCALS:
		return OS_TEXT("set local var by bin operator locals");

	case EXP_TYPE_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCAL_AND_NUMBER:
		return OS_TEXT("set local var by bin operator local & number");

	case EXP_TYPE_SET_ENV_VAR:
		return OS_TEXT("set env var");

	case EXP_TYPE_BIN_OPERATOR_BY_LOCALS:
		return OS_TEXT("binary operator by locals");

	case EXP_TYPE_BIN_OPERATOR_BY_LOCAL_AND_NUMBER:
		return OS_TEXT("binary operator by local & number");

	case EXP_TYPE_ASSIGN:
		return OS_TEXT("operator =");

	case EXP_TYPE_LOGIC_AND: // &&
		return OS_TEXT("logic &&");

	case EXP_TYPE_LOGIC_OR:  // ||
		return OS_TEXT("logic ||");

	case EXP_TYPE_LOGIC_PTR_EQ:  // ===
		return OS_TEXT("logic ===");

	case EXP_TYPE_LOGIC_PTR_NE:  // !==
		return OS_TEXT("logic !==");

	case EXP_TYPE_LOGIC_EQ:  // ==
		return OS_TEXT("logic ==");

	case EXP_TYPE_LOGIC_NE:  // !=
		return OS_TEXT("logic !=");

	case EXP_TYPE_LOGIC_GE:  // >=
		return OS_TEXT("logic >=");

	case EXP_TYPE_LOGIC_LE:  // <=
		return OS_TEXT("logic <=");

	case EXP_TYPE_LOGIC_GREATER: // >
		return OS_TEXT("logic >");

	case EXP_TYPE_LOGIC_LESS:    // <
		return OS_TEXT("logic <");

	case EXP_TYPE_LOGIC_BOOL:     // !!
		return OS_TEXT("logic bool");

	case EXP_TYPE_LOGIC_NOT:     // !
		return OS_TEXT("logic not");

	case EXP_TYPE_PLUS:
		return OS_TEXT("plus");

	case EXP_TYPE_NEG:
		return OS_TEXT("neg");

	case EXP_TYPE_LENGTH:
		return OS_TEXT("length");

	case EXP_TYPE_IN:
		return OS_TEXT("in");

	case EXP_TYPE_ISPROTOTYPEOF:
		return OS_TEXT("isprototypeof");

	case EXP_TYPE_IS:
		return OS_TEXT("is");

	case EXP_TYPE_SUPER:
		return OS_TEXT("super");
	/*
	case EXP_TYPE_TYPE_OF:
		return OS_TEXT("typeof");

	case EXP_TYPE_VALUE_OF:
		return OS_TEXT("valueof");

	case EXP_TYPE_NUMBER_OF:
		return OS_TEXT("numberof");

	case EXP_TYPE_STRING_OF:
		return OS_TEXT("stringof");

	case EXP_TYPE_ARRAY_OF:
		return OS_TEXT("arrayof");

	case EXP_TYPE_OBJECT_OF:
		return OS_TEXT("objectof");

	case EXP_TYPE_USERDATA_OF:
		return OS_TEXT("userdataof");

	case EXP_TYPE_FUNCTION_OF:
		return OS_TEXT("functionof");
	*/
		/*
		case EXP_TYPE_INC:     // ++
		return OS_TEXT("operator ++");

		case EXP_TYPE_DEC:     // --
		return OS_TEXT("operator --");
		*/

	case EXP_TYPE_PRE_INC:     // ++
		return OS_TEXT("pre ++");

	case EXP_TYPE_PRE_DEC:     // --
		return OS_TEXT("pre --");

	case EXP_TYPE_POST_INC:    // ++
		return OS_TEXT("post ++");

	case EXP_TYPE_POST_DEC:    // --
		return OS_TEXT("post --");

	case EXP_TYPE_BIT_AND: // &
		return OS_TEXT("bit &");

	case EXP_TYPE_BIT_OR:  // |
		return OS_TEXT("bit |");

	case EXP_TYPE_BIT_XOR: // ^
		return OS_TEXT("bit ^");

	case EXP_TYPE_BIT_NOT: // ~
		return OS_TEXT("bit ~");

	case EXP_TYPE_BIT_AND_ASSIGN: // &=
		return OS_TEXT("bit &=");

	case EXP_TYPE_BIT_OR_ASSIGN:  // |=
		return OS_TEXT("bit |=");

	case EXP_TYPE_BIT_XOR_ASSIGN: // ^=
		return OS_TEXT("bit ^=");

	case EXP_TYPE_BIT_NOT_ASSIGN: // ~=
		return OS_TEXT("bit ~=");

	case EXP_TYPE_CONCAT: // ..
		return OS_TEXT("operator ..");

	case EXP_TYPE_ADD: // +
		return OS_TEXT("operator +");

	case EXP_TYPE_SUB: // -
		return OS_TEXT("operator -");

	case EXP_TYPE_MUL: // *
		return OS_TEXT("operator *");

	case EXP_TYPE_DIV: // /
		return OS_TEXT("operator /");

	case EXP_TYPE_MOD: // %
		return OS_TEXT("operator %");

	case EXP_TYPE_LSHIFT: // <<
		return OS_TEXT("operator <<");

	case EXP_TYPE_RSHIFT: // >>
		return OS_TEXT("operator >>");

	case EXP_TYPE_POW: // **
		return OS_TEXT("operator **");

	case EXP_TYPE_ADD_ASSIGN: // +=
		return OS_TEXT("operator +=");

	case EXP_TYPE_SUB_ASSIGN: // -=
		return OS_TEXT("operator -=");

	case EXP_TYPE_MUL_ASSIGN: // *=
		return OS_TEXT("operator *=");

	case EXP_TYPE_DIV_ASSIGN: // /=
		return OS_TEXT("operator /=");

	case EXP_TYPE_MOD_ASSIGN: // %=
		return OS_TEXT("operator %=");

	case EXP_TYPE_LSHIFT_ASSIGN: // <<=
		return OS_TEXT("operator <<=");

	case EXP_TYPE_RSHIFT_ASSIGN: // >>=
		return OS_TEXT("operator >>=");

	case EXP_TYPE_POW_ASSIGN: // **=
		return OS_TEXT("operator **=");

	case EXP_TYPE_MOVE:
		return OS_TEXT("move");

	case EXP_TYPE_GET_XCONST:
		return OS_TEXT("get xconst");
	}
	return OS_TEXT("unknown exp");
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::Core::FunctionDecl::LocalVar::LocalVar(const String& p_name): name(p_name)
{
	start_code_pos = -1;
	end_code_pos = -1;
}

OS::Core::FunctionDecl::LocalVar::~LocalVar()
{
}

OS::Core::FunctionDecl::FunctionDecl() // Program * p_prog)
{
	// prog = p_prog;
#ifdef OS_DEBUG
	prog_func_index = -1;
#endif
	prog_parent_func_index = -1;
	locals = NULL;
	num_locals = 0;
	num_params = 0;
	max_up_count = 0;
	func_depth = 0;
	func_index = 0;
	num_local_funcs = 0;
	opcodes_pos = 0;
	opcodes_size = 0;
}

OS::Core::FunctionDecl::~FunctionDecl()
{
	OS_ASSERT(!locals); // && !prog);
}

// =====================================================================

OS::Core::Program::Program(OS * allocator): filename(allocator)
{
	this->allocator = allocator;
	ref_count = 1;
	opcodes = NULL;
	const_numbers = NULL;
	const_strings = NULL;
	num_numbers = 0;
	num_strings = 0;
	// gc_time = -1;
}

OS::Core::Program::~Program()
{
	OS_ASSERT(ref_count == 0);
	int i;
	// values could be already destroyed by gc or will be destroyed soon
	allocator->free(const_numbers);
	const_numbers = NULL;

	for(i = 0; i < num_strings; i++){
		OS_ASSERT(const_strings[i]->external_ref_count > 0);
		const_strings[i]->external_ref_count--;
		if(const_strings[i]->gc_color == GC_WHITE){
			const_strings[i]->gc_color = GC_BLACK;
		}
	}
	allocator->free(const_strings);
	const_strings = NULL;

	for(i = 0; i < num_functions; i++){
		FunctionDecl * func = functions + i;
		for(int j = 0; j < func->num_locals; j++){
			func->locals[j].~LocalVar();
		}
		allocator->free(func->locals);
		func->locals = NULL;
		// func->prog = NULL;
		func->~FunctionDecl();
	}
	allocator->free(functions);
	functions = NULL;

	allocator->deleteObj(opcodes);
	allocator->vectorClear(debug_info);
}

bool OS::Core::Compiler::saveToStream(StreamWriter * writer, StreamWriter * debug_info_writer)
{
	writer->writeBytes(OS_COMPILED_HEADER, OS_STRLEN(OS_COMPILED_HEADER));

	int i, len = OS_STRLEN(OS_VERSION)+1;
	writer->writeByte(len);
	writer->writeBytes(OS_VERSION, len);

	MemStreamWriter int_stream(allocator);
	MemStreamWriter float_stream(allocator);
	MemStreamWriter double_stream(allocator);
	int int_count = 0, float_count = 0, double_count = 0;
	int int_index = 0, float_index = 0, double_index = 0;
	for(i = 0; i < prog_numbers.count; i++){
		double val = prog_numbers[i];
		if(val >= 0 && (double)(int)val == val){
			int_count++;
			int_stream.writeUVariable(i - int_index); int_index = i;
			int_stream.writeUVariable((int)val);
			continue;
		}
		if((double)(float)val == val){
			float_count++;
			float_stream.writeUVariable(i - float_index); float_index = i;
			float_stream.writeFloat((float)val);
			continue;
		}
		double_count++;
		double_stream.writeUVariable(i - double_index); double_index = i;
		double_stream.writeDouble(val);
	}

	// writer->writeUVariable(prog_numbers.count);
	writer->writeUVariable(int_count);
	writer->writeUVariable(float_count);
	writer->writeUVariable(double_count);
	writer->writeUVariable(prog_strings.count);
	writer->writeUVariable(prog_functions.count);
	writer->writeUVariable(prog_opcodes_old->getPos());

	writer->writeBytes(int_stream.buffer.buf, int_stream.buffer.count);
	writer->writeBytes(float_stream.buffer.buf, float_stream.buffer.count);
	writer->writeBytes(double_stream.buffer.buf, double_stream.buffer.count);

	for(i = 0; i < prog_strings.count; i++){
		const String& str = prog_strings[i];
		int data_size = str.getDataSize();
		writer->writeUVariable(data_size);
		writer->writeBytes(str.toChar(), data_size);
	}
	for(i = 0; i < prog_functions.count; i++){
		Compiler::Scope * func_scope = prog_functions[i];
		writer->writeUVariable(func_scope->parent ? func_scope->parent->func_index+1 : 0); // prog_functions.indexOf(func_scope->parent));
		writer->writeUVariable(func_scope->num_locals);
		writer->writeUVariable(func_scope->num_params);
		writer->writeUVariable(func_scope->max_up_count);
		writer->writeUVariable(func_scope->func_depth);
		writer->writeUVariable(func_scope->func_index);
		writer->writeUVariable(func_scope->num_local_funcs);
		writer->writeUVariable(func_scope->opcodes_pos);
		writer->writeUVariable(func_scope->opcodes_size);

		OS_ASSERT(func_scope->locals_compiled.count == func_scope->num_locals);
		for(int j = 0; j < func_scope->locals_compiled.count; j++){
			Compiler::Scope::LocalVarCompiled& var_scope = func_scope->locals_compiled[j];
			OS_ASSERT(var_scope.start_code_pos >= func_scope->opcodes_pos && var_scope.start_code_pos < func_scope->opcodes_pos+func_scope->opcodes_size);
			OS_ASSERT(var_scope.end_code_pos > func_scope->opcodes_pos && var_scope.end_code_pos <= func_scope->opcodes_pos+func_scope->opcodes_size);
			writer->writeUVariable(var_scope.cached_name_index);
			writer->writeUVariable(var_scope.start_code_pos - func_scope->opcodes_pos);
			writer->writeUVariable(var_scope.end_code_pos - func_scope->opcodes_pos);
		}
	}

	writer->writeBytes(prog_opcodes_old->buffer.buf, prog_opcodes_old->buffer.count);

	if(debug_info_writer){
		debug_info_writer->writeBytes(OS_DEBUGINFO_HEADER, OS_STRLEN(OS_DEBUGINFO_HEADER));

		len = OS_STRLEN(OS_VERSION)+1;
		debug_info_writer->writeByte(len);
		debug_info_writer->writeBytes(OS_VERSION, len);

		debug_info_writer->writeUVariable(prog_debug_strings.count);
		debug_info_writer->writeUVariable(prog_num_debug_infos);

		for(i = 0; i < prog_debug_strings.count; i++){
			const String& str = prog_debug_strings[i];
			int data_size = str.getDataSize();
			debug_info_writer->writeUVariable(data_size);
			debug_info_writer->writeBytes(str.toChar(), data_size);
		}

		debug_info_writer->writeBytes(prog_debug_info->buffer.buf, prog_debug_info->buffer.count);
	}

	return true;
}

bool OS::Core::Program::loadFromStream(StreamReader * reader, StreamReader * debuginfo_reader)
{
	OS_ASSERT(!opcodes && !const_numbers && !num_numbers 
		&& !const_strings && !num_strings && !debug_info.count);

	int i, len = OS_STRLEN(OS_COMPILED_HEADER);
	if(!reader->checkBytes(OS_COMPILED_HEADER, len)){
		return false;
	}

	len = OS_STRLEN(OS_VERSION)+1;
	reader->movePos(1);
	if(!reader->checkBytes(OS_VERSION, len)){
		return false;
	}

	int int_count = reader->readUVariable();
	int float_count = reader->readUVariable();
	int double_count = reader->readUVariable();
	num_numbers = int_count + float_count + double_count;
	num_strings = reader->readUVariable();
	num_functions = reader->readUVariable();
	int opcodes_size = reader->readUVariable();

	const_numbers = (OS_NUMBER*)allocator->malloc(sizeof(OS_NUMBER) * num_numbers OS_DBG_FILEPOS);
	const_strings = (GCStringValue**)allocator->malloc(sizeof(GCStringValue*) * num_strings OS_DBG_FILEPOS);

	int num_index = 0;
	for(i = 0; i < int_count; i++){
		num_index += reader->readUVariable();
		OS_ASSERT(num_index >= 0 && num_index < num_numbers);
		OS_NUMBER number = (OS_NUMBER)reader->readUVariable();
		const_numbers[num_index] = number;
	}
	for(num_index = 0, i = 0; i < float_count; i++){
		num_index += reader->readUVariable();
		OS_ASSERT(num_index >= 0 && num_index < num_numbers);
		OS_NUMBER number = (OS_NUMBER)reader->readFloat();
		const_numbers[num_index] = number;
	}
	for(num_index = 0, i = 0; i < double_count; i++){
		num_index += reader->readUVariable();
		OS_ASSERT(num_index >= 0 && num_index < num_numbers);
		OS_NUMBER number = (OS_NUMBER)reader->readDouble();
		const_numbers[num_index] = number;
	}
	StringBuffer buf(allocator);
	for(i = 0; i < num_strings; i++){
		int data_size = reader->readUVariable();
		allocator->vectorReserveCapacity(buf, data_size/sizeof(OS_CHAR) + 1 OS_DBG_FILEPOS);
		reader->readBytes((void*)buf.buf, data_size);
		buf.count = data_size/sizeof(OS_CHAR);
		const_strings[i] = buf.toGCStringValue();
		const_strings[i]->external_ref_count++;
	}

	functions = (FunctionDecl*)allocator->malloc(sizeof(FunctionDecl) * num_functions OS_DBG_FILEPOS);
	for(i = 0; i < num_functions; i++){
		FunctionDecl * func = functions + i;
		new (func) FunctionDecl();
#ifdef OS_DEBUG
		func->prog_func_index = i;
#endif
		func->prog_parent_func_index = reader->readUVariable() - 1;
		func->num_locals = reader->readUVariable();
		func->num_params = reader->readUVariable();
		func->max_up_count = reader->readUVariable();
		func->func_depth = reader->readUVariable();
		func->func_index = reader->readUVariable();
		func->num_local_funcs = reader->readUVariable();
		func->opcodes_pos = reader->readUVariable();
		func->opcodes_size = reader->readUVariable();

		func->locals = (FunctionDecl::LocalVar*)allocator->malloc(sizeof(FunctionDecl::LocalVar) * func->num_locals OS_DBG_FILEPOS);
		for(int j = 0; j < func->num_locals; j++){
			int cached_name_index = reader->readUVariable();
			OS_ASSERT(cached_name_index >= 0 && cached_name_index < num_strings);
			FunctionDecl::LocalVar * local_var = func->locals + j;
			String var_name(const_strings[cached_name_index]);
			new (local_var) FunctionDecl::LocalVar(var_name);
			local_var->start_code_pos = reader->readUVariable() + func->opcodes_pos;
			local_var->end_code_pos = reader->readUVariable() + func->opcodes_pos;
		}
	}

	opcodes = new (allocator->malloc(sizeof(MemStreamReader) OS_DBG_FILEPOS)) MemStreamReader(allocator, opcodes_size);
	reader->readBytes(opcodes->buffer, opcodes_size);

	if(debuginfo_reader){
		len = OS_STRLEN(OS_DEBUGINFO_HEADER);
		if(!debuginfo_reader->checkBytes(OS_DEBUGINFO_HEADER, len)){
			return false;
		}

		len = OS_STRLEN(OS_VERSION)+1;
		debuginfo_reader->movePos(1);
		if(!debuginfo_reader->checkBytes(OS_VERSION, len)){
			return false;
		}
		int num_strings = debuginfo_reader->readUVariable();
		int num_debug_infos = debuginfo_reader->readUVariable();

		Vector<String> strings;
		allocator->vectorReserveCapacity(strings, num_strings OS_DBG_FILEPOS);

		StringBuffer buf(allocator);
		for(i = 0; i < num_strings; i++){
			int data_size = debuginfo_reader->readUVariable();
			allocator->vectorReserveCapacity(buf, data_size/sizeof(OS_CHAR) + 1 OS_DBG_FILEPOS);
			debuginfo_reader->readBytes((void*)buf.buf, data_size);
			buf.count = data_size/sizeof(OS_CHAR);
			allocator->vectorAddItem(strings, buf.toString() OS_DBG_FILEPOS);
		}

		allocator->vectorReserveCapacity(debug_info, num_debug_infos OS_DBG_FILEPOS);
		for(i = 0; i < num_debug_infos; i++){
			int end_opcode_offs = debuginfo_reader->readUVariable();
			int line = debuginfo_reader->readUVariable();
			int pos = debuginfo_reader->readUVariable();
			int string_index = debuginfo_reader->readUVariable();
			allocator->vectorAddItem(debug_info, DebugInfoItem(end_opcode_offs, line, pos, strings[string_index]) OS_DBG_FILEPOS);
		}
		allocator->vectorClear(strings);
	}

	return true;
}

OS::Core::Program::DebugInfoItem * OS::Core::Program::getDebugInfo(int opcode_pos)
{
	Program::DebugInfoItem * info = NULL;
	for(int i = 0; i < debug_info.count; i++){
		Program::DebugInfoItem * cur = &debug_info[i];
		if(cur->opcode_pos < opcode_pos){
			info = cur;
		}
		if(cur->opcode_pos > opcode_pos){
			break;
		}
	}
	return info;
}

OS::Core::Program::DebugInfoItem::DebugInfoItem(int p_opcode_pos, int p_line, int p_pos, const String& p_token): token(p_token)
{
	opcode_pos = p_opcode_pos;
	line = p_line;
	pos = p_pos;
}

void OS::Core::Program::pushStartFunction()
{
	int opcode = opcodes->readByte();
	if(opcode != OP_PUSH_FUNCTION){
		OS_ASSERT(false);
		allocator->pushNull();
		return;
	}

	int prog_func_index = opcodes->readUVariable();
	OS_ASSERT(prog_func_index == 0); // func_index >= 0 && func_index < num_functions);
	FunctionDecl * func_decl = functions + prog_func_index;
	OS_ASSERT(func_decl->max_up_count == 0);

	GCFunctionValue * func_value = allocator->core->newFunctionValue(NULL, this, func_decl, allocator->core->global_vars);
	allocator->core->pushValue(func_value);
	if(filename.getDataSize()){
		StringBuffer buf(allocator);
		buf += OS_TEXT("<<");
		buf += allocator->getFilename(filename);
		buf += OS_TEXT(">>");
		func_value->name = buf.toGCStringValue();
	}else{
		func_value->name = OS::String(allocator, OS_TEXT("<<CORE>>")).string;
	}

	allocator->core->gcMarkProgram(this);

	OS_ASSERT(func_decl->opcodes_pos == opcodes->getPos());
	opcodes->movePos(func_decl->opcodes_size);
}

OS::Core::Program * OS::Core::Program::retain()
{
	ref_count++;
	return this;
}

void OS::Core::Program::release()
{
	if(--ref_count <= 0){
		OS_ASSERT(ref_count == 0);
		OS * allocator = this->allocator;
		this->~Program();
		allocator->free(this);
	}
}

OS::Core::Program::OpcodeType OS::Core::Program::getOpcodeType(Compiler::ExpressionType exp_type)
{
	switch(exp_type){
	case Compiler::EXP_TYPE_CALL: return OP_CALL;
	case Compiler::EXP_TYPE_CALL_AUTO_PARAM: return OP_CALL;
		// case Compiler::EXP_TYPE_GET_DIM: return OP_GET_DIM;
	case Compiler::EXP_TYPE_CALL_METHOD: return OP_CALL_METHOD;
	case Compiler::EXP_TYPE_TAIL_CALL: return OP_TAIL_CALL;
	case Compiler::EXP_TYPE_TAIL_CALL_METHOD: return OP_TAIL_CALL_METHOD;

	case Compiler::EXP_TYPE_GET_THIS: return OP_PUSH_THIS;
	case Compiler::EXP_TYPE_GET_ARGUMENTS: return OP_PUSH_ARGUMENTS;
	case Compiler::EXP_TYPE_GET_REST_ARGUMENTS: return OP_PUSH_REST_ARGUMENTS;

	case Compiler::EXP_TYPE_SUPER: return OP_SUPER;
	/*
	case Compiler::EXP_TYPE_TYPE_OF: return OP_TYPE_OF;
	case Compiler::EXP_TYPE_VALUE_OF: return OP_VALUE_OF;
	case Compiler::EXP_TYPE_NUMBER_OF: return OP_NUMBER_OF;
	case Compiler::EXP_TYPE_STRING_OF: return OP_STRING_OF;
	case Compiler::EXP_TYPE_ARRAY_OF: return OP_ARRAY_OF;
	case Compiler::EXP_TYPE_OBJECT_OF: return OP_OBJECT_OF;
	case Compiler::EXP_TYPE_USERDATA_OF: return OP_USERDATA_OF;
	case Compiler::EXP_TYPE_FUNCTION_OF: return OP_FUNCTION_OF;
	*/
	case Compiler::EXP_TYPE_LOGIC_BOOL: return OP_LOGIC_BOOL;
	case Compiler::EXP_TYPE_LOGIC_NOT: return OP_LOGIC_NOT;
	case Compiler::EXP_TYPE_BIT_NOT: return OP_BIT_NOT;
	case Compiler::EXP_TYPE_PLUS: return OP_PLUS;
	case Compiler::EXP_TYPE_NEG: return OP_NEG;
	case Compiler::EXP_TYPE_LENGTH: return OP_LENGTH;

	case Compiler::EXP_TYPE_CONCAT: return OP_CONCAT;
	case Compiler::EXP_TYPE_IN: return OP_IN;
	case Compiler::EXP_TYPE_ISPROTOTYPEOF: return OP_ISPROTOTYPEOF;
	case Compiler::EXP_TYPE_IS: return OP_IS;

	case Compiler::EXP_TYPE_LOGIC_AND: return OP_LOGIC_AND_4;
	case Compiler::EXP_TYPE_LOGIC_OR: return OP_LOGIC_OR_4;
	case Compiler::EXP_TYPE_LOGIC_PTR_EQ: return OP_LOGIC_PTR_EQ;
	case Compiler::EXP_TYPE_LOGIC_PTR_NE: return OP_LOGIC_PTR_NE;
	case Compiler::EXP_TYPE_LOGIC_EQ: return OP_LOGIC_EQ;
	case Compiler::EXP_TYPE_LOGIC_NE: return OP_LOGIC_NE;
	case Compiler::EXP_TYPE_LOGIC_GE: return OP_LOGIC_GE;
	case Compiler::EXP_TYPE_LOGIC_LE: return OP_LOGIC_LE;
	case Compiler::EXP_TYPE_LOGIC_GREATER: return OP_LOGIC_GREATER;
	case Compiler::EXP_TYPE_LOGIC_LESS: return OP_LOGIC_LESS;

	case Compiler::EXP_TYPE_BIT_AND: return OP_BIT_AND;
	case Compiler::EXP_TYPE_BIT_OR: return OP_BIT_OR;
	case Compiler::EXP_TYPE_BIT_XOR: return OP_BIT_XOR;

	case Compiler::EXP_TYPE_ADD: return OP_ADD;
	case Compiler::EXP_TYPE_SUB: return OP_SUB;
	case Compiler::EXP_TYPE_MUL: return OP_MUL;
	case Compiler::EXP_TYPE_DIV: return OP_DIV;
	case Compiler::EXP_TYPE_MOD: return OP_MOD;
	case Compiler::EXP_TYPE_LSHIFT: return OP_LSHIFT;
	case Compiler::EXP_TYPE_RSHIFT: return OP_RSHIFT;
	case Compiler::EXP_TYPE_POW: return OP_POW;
	}
	OS_ASSERT(false);
	return OP_UNKNOWN;
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::Core::StreamWriter::StreamWriter(OS * p_allocator)
{
	allocator = p_allocator;
}

OS::Core::StreamWriter::~StreamWriter()
{
}

void OS::Core::StreamWriter::writeFromStream(StreamReader * reader)
{
	int size = reader->getSize() - reader->getPos();
	int buf_size = 1024 * 16;
	void * buf = allocator->malloc(buf_size < size ? buf_size : size OS_DBG_FILEPOS);
	OS_ASSERT(buf || !size);
	for(; size > 0; size -= buf_size){
		int chunk_size = buf_size <= size ? buf_size : size;
		reader->readBytes(buf, chunk_size);
		writeBytes(buf, chunk_size);
	}
	allocator->free(buf);
}

void OS::Core::StreamWriter::writeByte(int value)
{
	OS_ASSERT(value >= 0 && value <= 0xff);
	OS_BYTE le_value = toLittleEndianByteOrder((OS_BYTE)value);
	writeBytes(&le_value, sizeof(le_value));
}

void OS::Core::StreamWriter::writeByteAtPos(int value, int pos)
{
	OS_ASSERT(value >= 0 && value <= 0xff);
	OS_BYTE le_value = toLittleEndianByteOrder((OS_BYTE)value);
	writeBytesAtPos(&le_value, sizeof(le_value), pos);
}

void OS::Core::StreamWriter::writeUVariable(int value)
{
	OS_ASSERT(value >= 0);
	for(;;){
		if(value >= 0x7f){
			writeByte((value & 0x7f) | 0x80);
			value >>= 7;
		}else{
			writeByte(value);
			return;
		}
	}
}

void OS::Core::StreamWriter::writeU16(int value)
{
	OS_ASSERT(value >= 0 && value <= 0xffff);
	OS_U16 le_value = toLittleEndianByteOrder((OS_U16)value);
	writeBytes(&le_value, sizeof(le_value));
}

void OS::Core::StreamWriter::writeU16AtPos(int value, int pos)
{
	OS_ASSERT(value >= 0 && value <= 0xffff);
	OS_U16 le_value = toLittleEndianByteOrder((OS_U16)value);
	writeBytesAtPos(&le_value, sizeof(le_value), pos);
}

void OS::Core::StreamWriter::writeInt8(int value)
{
	OS_ASSERT((int)(OS_INT8)value == value);
	OS_INT8 le_value = toLittleEndianByteOrder((OS_INT8)value);
	writeBytes(&le_value, sizeof(le_value));
}

void OS::Core::StreamWriter::writeInt8AtPos(int value, int pos)
{
	OS_ASSERT((int)(OS_INT8)value == value);
	OS_INT8 le_value = toLittleEndianByteOrder((OS_INT8)value);
	writeBytesAtPos(&le_value, sizeof(le_value), pos);
}

void OS::Core::StreamWriter::writeInt16(int value)
{
	OS_ASSERT((int)(OS_INT16)value == value);
	OS_INT16 le_value = toLittleEndianByteOrder((OS_INT16)value);
	writeBytes(&le_value, sizeof(le_value));
}

void OS::Core::StreamWriter::writeInt16AtPos(int value, int pos)
{
	OS_ASSERT((int)(OS_INT16)value == value);
	OS_INT16 le_value = toLittleEndianByteOrder((OS_INT16)value);
	writeBytesAtPos(&le_value, sizeof(le_value), pos);
}

void OS::Core::StreamWriter::writeInt32(int value)
{
	OS_INT32 le_value = toLittleEndianByteOrder((OS_INT32)value);
	writeBytes(&le_value, sizeof(le_value));
}

void OS::Core::StreamWriter::writeInt32AtPos(int value, int pos)
{
	OS_ASSERT((int)(OS_INT32)value == value);
	OS_INT32 le_value = toLittleEndianByteOrder((OS_INT32)value);
	writeBytesAtPos(&le_value, sizeof(le_value), pos);
}

void OS::Core::StreamWriter::writeInt64(OS_INT64 value)
{
	OS_INT64 le_value = toLittleEndianByteOrder((OS_INT64)value);
	writeBytes(&le_value, sizeof(le_value));
}

void OS::Core::StreamWriter::writeInt64AtPos(OS_INT64 value, int pos)
{
	OS_INT64 le_value = toLittleEndianByteOrder((OS_INT64)value);
	writeBytesAtPos(&le_value, sizeof(le_value), pos);
}

void OS::Core::StreamWriter::writeFloat(float value)
{
	float le_value = toLittleEndianByteOrder(value);
	writeBytes(&le_value, sizeof(le_value));
}

void OS::Core::StreamWriter::writeFloatAtPos(float value, int pos)
{
	float le_value = toLittleEndianByteOrder(value);
	writeBytesAtPos(&le_value, sizeof(le_value), pos);
}

void OS::Core::StreamWriter::writeDouble(double value)
{
	double le_value = toLittleEndianByteOrder(value);
	writeBytes(&le_value, sizeof(le_value));
}

void OS::Core::StreamWriter::writeDoubleAtPos(double value, int pos)
{
	double le_value = toLittleEndianByteOrder(value);
	writeBytesAtPos(&le_value, sizeof(le_value), pos);
}

// =====================================================================

OS::Core::MemStreamWriter::MemStreamWriter(OS * allocator): StreamWriter(allocator)
{
	pos = 0;
}

OS::Core::MemStreamWriter::~MemStreamWriter()
{
	allocator->vectorClear(buffer);
}

int OS::Core::MemStreamWriter::getPos() const
{
	return pos;
}

void OS::Core::MemStreamWriter::setPos(int new_pos)
{
	OS_ASSERT(new_pos >= 0 && new_pos <= buffer.count);
	pos = new_pos;
}

int OS::Core::MemStreamWriter::getSize() const
{
	return buffer.count;
}

void OS::Core::MemStreamWriter::writeBytes(const void * buf, int len)
{
	// int pos = buffer.count;
	allocator->vectorReserveCapacity(buffer, pos + len OS_DBG_FILEPOS);
	int save_pos = pos;
	pos += len;
	if(buffer.count <= pos){
		buffer.count = pos;
	}
	writeBytesAtPos(buf, len, save_pos);
}

void OS::Core::MemStreamWriter::writeBytesAtPos(const void * buf, int len, int pos)
{
	OS_ASSERT(pos >= 0 && pos <= buffer.count-len);
	OS_MEMCPY(buffer.buf+pos, buf, len);
}

void OS::Core::MemStreamWriter::writeByte(int value)
{
	OS_ASSERT(value >= 0 && value <= 0xff);
	if(pos < buffer.count){
		OS_ASSERT(pos >= 0);
		buffer[pos++] = (OS_BYTE)value;
	}else{
		allocator->vectorAddItem(buffer, (OS_BYTE)value OS_DBG_FILEPOS);
		pos++;
	}
}

void OS::Core::MemStreamWriter::writeByteAtPos(int value, int pos)
{
	OS_ASSERT(value >= 0 && value <= 0xff);
	OS_ASSERT(pos >= 0 && pos <= buffer.count-1);
	buffer[pos] = (OS_BYTE)value;
}

// =====================================================================

OS::Core::FileStreamWriter::FileStreamWriter(OS * allocator, const OS_CHAR * filename): StreamWriter(allocator)
{
	f = allocator->openFile(filename, "wb");
}

OS::Core::FileStreamWriter::~FileStreamWriter()
{
	allocator->closeFile(f);
}

int OS::Core::FileStreamWriter::getPos() const
{
	return allocator->seekFile(f, 0, SEEK_CUR);
}

void OS::Core::FileStreamWriter::setPos(int new_pos)
{
	OS_ASSERT(new_pos >= 0 && new_pos <= getSize());
	allocator->seekFile(f, new_pos, SEEK_SET);
}

int OS::Core::FileStreamWriter::getSize() const
{
	if(f){
		int save_pos = getPos();
		allocator->seekFile(f, 0, SEEK_END);
		int size = getPos();
		allocator->seekFile(f, save_pos, SEEK_SET);
		return size;
	}
	return 0;
}

void OS::Core::FileStreamWriter::writeBytes(const void * buf, int len)
{
	allocator->writeFile(buf, len, f);
}

void OS::Core::FileStreamWriter::writeBytesAtPos(const void * buf, int len, int pos)
{
	int save_pos = getPos();
	allocator->seekFile(f, pos, SEEK_SET);
	writeBytes(buf, len);
	allocator->seekFile(f, save_pos, SEEK_SET);
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::Core::StreamReader::StreamReader(OS * p_allocator)
{
	allocator = p_allocator;
}

OS::Core::StreamReader::~StreamReader()
{
}

OS_BYTE OS::Core::StreamReader::readByte()
{
	OS_BYTE le_value;
	readBytes(&le_value, sizeof(le_value));
	return fromLittleEndianByteOrder(le_value);
}

OS_BYTE OS::Core::StreamReader::readByteAtPos(int pos)
{
	OS_BYTE le_value;
	readBytesAtPos(&le_value, sizeof(le_value), pos);
	return fromLittleEndianByteOrder(le_value);
}

int OS::Core::StreamReader::readUVariable()
{
	int value = readByte();
	if(!(value & 0x80)){
		return value;
	}
	value &= 0x7f;
	for(int i = 7;; i += 7){
		int b = readByte();
		if(b & 0x80){
			value |= (b & 0x7f) << i;
		}else{
			OS_ASSERT((value | (b << i)) >= 0);
			return value | (b << i);
		}
	}
	return 0; // shut up compiler
}

OS_U16 OS::Core::StreamReader::readU16()
{
	OS_U16 le_value;
	readBytes(&le_value, sizeof(le_value));
	return fromLittleEndianByteOrder(le_value);
}

OS_U16 OS::Core::StreamReader::readU16AtPos(int pos)
{
	OS_U16 le_value;
	readBytesAtPos(&le_value, sizeof(le_value), pos);
	return fromLittleEndianByteOrder(le_value);
}

OS_INT8 OS::Core::StreamReader::readInt8()
{
	OS_INT8 le_value;
	readBytes(&le_value, sizeof(le_value));
	return fromLittleEndianByteOrder(le_value);
}

OS_INT8 OS::Core::StreamReader::readInt8AtPos(int pos)
{
	OS_INT8 le_value;
	readBytesAtPos(&le_value, sizeof(le_value), pos);
	return fromLittleEndianByteOrder(le_value);
}

OS_INT16 OS::Core::StreamReader::readInt16()
{
	OS_INT16 le_value;
	readBytes(&le_value, sizeof(le_value));
	return fromLittleEndianByteOrder(le_value);
}

OS_INT16 OS::Core::StreamReader::readInt16AtPos(int pos)
{
	OS_INT16 le_value;
	readBytesAtPos(&le_value, sizeof(le_value), pos);
	return fromLittleEndianByteOrder(le_value);
}

OS_INT32 OS::Core::StreamReader::readInt32()
{
	OS_INT32 le_value;
	readBytes(&le_value, sizeof(le_value));
	return fromLittleEndianByteOrder(le_value);
}

OS_INT32 OS::Core::StreamReader::readInt32AtPos(int pos)
{
	OS_INT32 le_value;
	readBytesAtPos(&le_value, sizeof(le_value), pos);
	return fromLittleEndianByteOrder(le_value);
}

OS_INT64 OS::Core::StreamReader::readInt64()
{
	OS_INT64 le_value;
	readBytes(&le_value, sizeof(le_value));
	return fromLittleEndianByteOrder(le_value);
}

OS_INT64 OS::Core::StreamReader::readInt64AtPos(int pos)
{
	OS_INT64 le_value;
	readBytesAtPos(&le_value, sizeof(le_value), pos);
	return fromLittleEndianByteOrder(le_value);
}

float OS::Core::StreamReader::readFloat()
{
	float le_value;
	readBytes(&le_value, sizeof(le_value));
	return fromLittleEndianByteOrder(le_value);
}

float OS::Core::StreamReader::readFloatAtPos(int pos)
{
	float le_value;
	readBytesAtPos(&le_value, sizeof(le_value), pos);
	return fromLittleEndianByteOrder(le_value);
}

double OS::Core::StreamReader::readDouble()
{
	double le_value;
	readBytes(&le_value, sizeof(le_value));
	return fromLittleEndianByteOrder(le_value);
}

double OS::Core::StreamReader::readDoubleAtPos(int pos)
{
	double le_value;
	readBytesAtPos(&le_value, sizeof(le_value), pos);
	return fromLittleEndianByteOrder(le_value);
}

// =====================================================================

OS::Core::MemStreamReader::MemStreamReader(OS * allocator, int buf_size): StreamReader(allocator)
{
	cur = buffer = (OS_BYTE*)allocator->malloc(buf_size OS_DBG_FILEPOS);
	size = buf_size;
}

OS::Core::MemStreamReader::MemStreamReader(OS * allocator, OS_BYTE * buf, int buf_size): StreamReader(allocator)
{
	cur = buffer = buf;
	size = buf_size;
}

OS::Core::MemStreamReader::~MemStreamReader()
{
	if(allocator){
		allocator->free(buffer);
	}
}

int OS::Core::MemStreamReader::getPos() const
{
	return cur - buffer;
}

void OS::Core::MemStreamReader::setPos(int new_pos)
{
	OS_ASSERT(new_pos >= 0 && new_pos <= size);
	cur = buffer + new_pos;
}

int OS::Core::MemStreamReader::getSize() const
{
	return size;
}

void OS::Core::MemStreamReader::movePos(int len)
{
	OS_ASSERT(getPos()+len >= 0 && getPos()+len <= size);
	cur += len;
}

bool OS::Core::MemStreamReader::checkBytes(const void * src, int len)
{
	OS_ASSERT(getPos() >= 0 && getPos()+len <= size);
	bool r = OS_MEMCMP(cur, src, len) == 0;
	cur += len;
	return r;
}

void * OS::Core::MemStreamReader::readBytes(void * dst, int len)
{
	OS_ASSERT(getPos() >= 0 && getPos()+len <= size);
	OS_MEMCPY(dst, cur, len);
	cur += len;
	return dst;
}

void * OS::Core::MemStreamReader::readBytesAtPos(void * dst, int len, int pos)
{
	OS_ASSERT(pos >= 0 && pos+len <= size);
	OS_MEMCPY(dst, buffer + pos, len);
	return dst;
}

OS_BYTE OS::Core::MemStreamReader::readByte()
{
	OS_ASSERT(getPos() >= 0 && getPos()+(int)sizeof(OS_BYTE) <= size);
	return *cur++;
}

OS_BYTE OS::Core::MemStreamReader::readByteAtPos(int pos)
{
	OS_ASSERT(pos >= 0 && pos+(int)sizeof(OS_BYTE) <= size);
	return buffer[pos];
}

OS_INT8 OS::Core::MemStreamReader::readInt8()
{
	OS_ASSERT(getPos() >= 0 && getPos()+1 <= size);
	return (OS_INT8)*cur++;
}

OS_INT16 OS::Core::MemStreamReader::readInt16()
{
	OS_ASSERT(getPos() >= 0 && getPos()+(int)sizeof(OS_INT16) <= size);
	OS_BYTE * buf = cur;
	cur += sizeof(OS_INT16);
	OS_INT16 value = buf[0] | (buf[1] << 8);
	return value;
}

OS_INT32 OS::Core::MemStreamReader::readInt32()
{
	OS_ASSERT(getPos() >= 0 && getPos()+(int)sizeof(OS_INT32) <= size);
	OS_BYTE * buf = cur;
	cur += sizeof(OS_INT32);
	OS_INT32 value = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
	return value;
}

// =====================================================================

OS::Core::FileStreamReader::FileStreamReader(OS * allocator, const OS_CHAR * filename): StreamReader(allocator)
{
	f = allocator->openFile(filename, "rb");
}

OS::Core::FileStreamReader::~FileStreamReader()
{
	allocator->closeFile(f);
}

int OS::Core::FileStreamReader::getPos() const
{
	return allocator->seekFile(f, 0, SEEK_CUR);
}

void OS::Core::FileStreamReader::setPos(int new_pos)
{
	OS_ASSERT(new_pos >= 0 && new_pos <= getSize());
	allocator->seekFile(f, new_pos, SEEK_SET);
}

int OS::Core::FileStreamReader::getSize() const
{
	if(f){
		int save_pos = getPos();
		allocator->seekFile(f, 0, SEEK_END);
		int size = getPos();
		allocator->seekFile(f, save_pos, SEEK_SET);
		return size;
	}
	return 0;
}

void OS::Core::FileStreamReader::movePos(int len)
{
	allocator->seekFile(f, len, SEEK_CUR);
}

bool OS::Core::FileStreamReader::checkBytes(const void * src, int len)
{
	void * buf = alloca(len);
	readBytes(buf, len);
	return OS_MEMCMP(buf, src, len) == 0;
}

void * OS::Core::FileStreamReader::readBytes(void * buf, int len)
{
	if(!f || !allocator->readFile(buf, len, f)){
		OS_MEMSET(buf, 0, len);
	}
	return buf;
}

void * OS::Core::FileStreamReader::readBytesAtPos(void * buf, int len, int pos)
{
	int save_pos = getPos();
	allocator->seekFile(f, pos, SEEK_SET);
	readBytes(buf, len);
	allocator->seekFile(f, save_pos, SEEK_SET);
	return buf;
}

// =====================================================================
// =====================================================================
// =====================================================================

static bool isDecString(const OS_CHAR * str, int len)
{
	OS_INT val;
	const OS_CHAR * end = str + len;
	return parseSimpleDec(str, val) && str == end;
}

OS::Core::PropertyIndex::PropertyIndex(const PropertyIndex& p_index): index(p_index.index)
{
}

OS::Core::PropertyIndex::PropertyIndex(Value p_index): index(p_index)
{
	convertIndexStringToNumber();
}

OS::Core::PropertyIndex::PropertyIndex(Value p_index, const KeepStringIndex&): index(p_index)
{
	OS_ASSERT(index.type != OS_VALUE_TYPE_STRING || PropertyIndex(p_index).index.type == OS_VALUE_TYPE_STRING);
}

OS::Core::PropertyIndex::PropertyIndex(GCStringValue * p_index): index(p_index)
{
	convertIndexStringToNumber();
}

OS::Core::PropertyIndex::PropertyIndex(GCStringValue * p_index, const KeepStringIndex&): index(p_index)
{
	// OS_ASSERT(index.type != OS_VALUE_TYPE_STRING || PropertyIndex(p_index).index.type == OS_VALUE_TYPE_STRING);
}

OS::Core::PropertyIndex::PropertyIndex(const String& p_index): index(p_index.string)
{
	convertIndexStringToNumber();
}

OS::Core::PropertyIndex::PropertyIndex(const String& p_index, const KeepStringIndex&): index(p_index.string)
{
	// OS_ASSERT(index.type != OS_VALUE_TYPE_STRING || PropertyIndex(p_index).index.type == OS_VALUE_TYPE_STRING);
}

void OS::Core::PropertyIndex::convertIndexStringToNumber()
{
	if(index.type == OS_VALUE_TYPE_STRING){
		bool neg = false;
		const OS_CHAR * str = index.v.string->toChar();
		if((*str >= OS_TEXT('0') && *str <= OS_TEXT('9'))
			|| ((neg = *str == OS_TEXT('-')) && str[1] >= OS_TEXT('0') && str[1] <= OS_TEXT('9')))
		{
			const OS_CHAR * end = str + index.v.string->getLen();
			str += (int)neg;
			OS_FLOAT val;
			if(parseSimpleFloat(str, val)){
				if(*str == OS_TEXT('.')){
					OS_FLOAT m = (OS_FLOAT)0.1;
					for(str++; *str >= OS_TEXT('0') && *str <= OS_TEXT('9'); str++, m *= (OS_FLOAT)0.1){
						val += (OS_FLOAT)(*str - OS_TEXT('0')) * m;
					}
				}
				if(str == end){
					index.v.number = (OS_NUMBER)(neg ? -val : val);
					index.type = OS_VALUE_TYPE_NUMBER;
					// OS_ASSERT((OS_INT)index.v.number == val);
				}
			}
		}
	}
}

bool OS::Core::PropertyIndex::isEqual(const PropertyIndex& b) const
{
	switch(index.type){
	case OS_VALUE_TYPE_NULL:
		return b.index.type == OS_VALUE_TYPE_NULL;

		// case OS_VALUE_TYPE_BOOL:
		//	return b.index.type == OS_VALUE_TYPE_BOOL && index.v.boolean == b.index.v.boolean;

	case OS_VALUE_TYPE_NUMBER:
		return b.index.type == OS_VALUE_TYPE_NUMBER && index.v.number == b.index.v.number;
	}
	return index.type == b.index.type && index.v.value == b.index.v.value;
}

bool OS::Core::GCStringValue::isEqual(int hash, const void * b, int size) const
{
	return this->hash == hash 
		&& data_size == size
		&& OS_MEMCMP(toMemory(), b, size) == 0;
}

bool OS::Core::GCStringValue::isEqual(int hash, const void * buf1, int size1, const void * buf2, int size2) const
{
	if(this->hash != hash || data_size != size1 + size2){
		return false;
	}
	const OS_BYTE * src = toBytes();
	return OS_MEMCMP(src, buf1, size1) == 0
		&& (!size2 || OS_MEMCMP(src + size1, buf2, size2) == 0);
}

bool OS::Core::PropertyIndex::isEqual(int hash, const void * b, int size) const
{
	if(index.type == OS_VALUE_TYPE_STRING){
		return index.v.string->hash == hash 
			&& index.v.string->data_size == size
			&& OS_MEMCMP(index.v.string->toMemory(), b, size) == 0;
	}
	return false;
}

bool OS::Core::PropertyIndex::isEqual(int hash, const void * buf1, int size1, const void * buf2, int size2) const
{
	if(index.type == OS_VALUE_TYPE_STRING){
		int src_size = index.v.string->data_size;
		if(index.v.string->hash != hash || src_size != size1 + size2){
			return false;
		}
		const OS_BYTE * src = index.v.string->toBytes();
		return Utils::cmp(src, size1, buf1, size1) == 0
			&& Utils::cmp(src + size1, size2, buf2, size2) == 0;
	}
	return false;
}

template <class T> int getNumberHash(T val)
{
	return (int)val;
}
template <> int getNumberHash<double>(double val)
{
	float t = (float)val;
	return *(int*)&t;
}
template <> int getNumberHash<float>(float t)
{
	return *(int*)&t;
}
template <> int getNumberHash<int>(int t)
{
	return t;
}

int OS::Core::PropertyIndex::getHash() const
{
	switch(index.type){
		/*
		case OS_VALUE_TYPE_NULL:
		return 0;

		case OS_VALUE_TYPE_BOOL:
		return index.v.boolean;
		*/

	case OS_VALUE_TYPE_NUMBER:
#if 1 // speed optimization
		// return getNumberHash(index.v.number);
		{
			union { 
				double d; 
				OS_INT32 p[2];
			} u;
			u.d = (double)index.v.number; // + 1.0f;
			return u.p[0] + u.p[1];
		}
#else
		/* if(sizeof(index.v.number) > sizeof(float)){
			float t = (float)index.v.number;
			return *(int*)&t;
		} */
		// return (int)index.v.number;
		OS_ASSERT(sizeof(int) <= sizeof(index.v.number));
		if(IS_LITTLE_ENDIAN){
			OS_U32 t = ((OS_U32*)((OS_BYTE*)&index.v.number + sizeof(index.v.number)))[-1];
			return (t>>24) | (t<<8);
			// return ((int*)((OS_BYTE*)&index.v.number + sizeof(index.v.number)))[-1];
		}else{
			OS_U32 t = *(OS_U32*)&index.v.number;
			return (t>>24) | (t<<8);
		}
#endif

	case OS_VALUE_TYPE_STRING:
		return index.v.string->hash;
	}
	// all other values share same area with index.v.value so just use it as hash
	return (ptrdiff_t) index.v.value;
}

// =====================================================================

OS::Core::Property::Property(const PropertyIndex& index): PropertyIndex(index)
{
	hash_next = NULL;
	prev = NULL;
	next = NULL;
}

OS::Core::Property::Property(Value index): PropertyIndex(index)
{
	hash_next = NULL;
	prev = NULL;
	next = NULL;
}

OS::Core::Property::Property(Value index, const KeepStringIndex& keep): PropertyIndex(index, keep)
{
	hash_next = NULL;
	prev = NULL;
	next = NULL;
}

OS::Core::Property::Property(GCStringValue * index): PropertyIndex(index)
{
	hash_next = NULL;
	prev = NULL;
	next = NULL;
}

OS::Core::Property::Property(GCStringValue * index, const KeepStringIndex& keep): PropertyIndex(index, keep)
{
	hash_next = NULL;
	prev = NULL;
	next = NULL;
}

OS::Core::Property::~Property()
{
	OS_ASSERT(!hash_next);
	OS_ASSERT(!prev);
	OS_ASSERT(!next);
}

// =====================================================================

OS::Core::Table::IteratorState::IteratorState()
{
	table = NULL;
	prop = NULL;
	next = NULL;
	ascending = true;
}

OS::Core::Table::IteratorState::~IteratorState()
{
	OS_ASSERT(!table && !prop && !next);
}

OS::Core::Table::Table()
{
	head_mask = 0;
	heads = NULL;
	next_index = 0;
	count = 0;
	first = last = NULL;
	iterators = NULL;
}

OS::Core::Table::~Table()
{
	OS_ASSERT(count == 0 && !first && !last && !iterators);
	OS_ASSERT(!heads);
}

bool OS::Core::Table::containsIterator(IteratorState * iter)
{
	for(IteratorState * cur = iterators; cur; cur = cur->next){
		if(cur == iter){
			OS_ASSERT(iter->table == this);
			return true;
		}
	}
	OS_ASSERT(iter->table != this);
	return false;
}

void OS::Core::Table::addIterator(IteratorState * iter)
{
	OS_ASSERT(!containsIterator(iter));
	OS_ASSERT(!iter->prop && !iter->table);
	iter->table = this;
	iter->prop = iter->ascending ? first : last;
	iter->next = iterators;
	iterators = iter;
}

void OS::Core::Table::removeIterator(IteratorState * iter)
{
	OS_ASSERT(containsIterator(iter));
	IteratorState * prev = NULL;
	for(IteratorState * cur = iterators; cur; prev = cur, cur = cur->next){
		if(cur == iter){
			if(!prev){
				iterators = cur->next;
			}else{
				prev->next = cur->next;
			}
			cur->table = NULL;
			cur->next = NULL;
			cur->prop = NULL;
			return;
		}
	}
	OS_ASSERT(false);
}

OS::Core::Table * OS::Core::newTable(OS_DBG_FILEPOS_START_DECL)
{
	return new (malloc(sizeof(Table) OS_DBG_FILEPOS_PARAM)) Table();
}

void OS::Core::clearTable(Table * table)
{
	OS_ASSERT(table);
	Property * prop = table->last, * prev;

	table->count = 0;
	table->first = NULL;
	table->last = NULL;

	for(; prop; prop = prev){
		prev = prop->prev;
		prop->hash_next = NULL;
		prop->prev = NULL;
		prop->next = NULL;
		prop->~Property();
		free(prop);
	}

	while(table->iterators){
		table->removeIterator(table->iterators);
	}

	// OS_ASSERT(table->count == 0 && !table->first && !table->last);
	free(table->heads);
	table->heads = NULL;
	table->head_mask = 0;
	table->next_index = 0;
}

void OS::Core::deleteTable(Table * table)
{
	OS_ASSERT(table);
	clearTable(table);
	table->~Table();
	free(table);
}

void OS::Core::addTableProperty(Table * table, Property * prop)
{
	OS_ASSERT(prop->next == NULL);
	OS_ASSERT(!table->get(*prop));

	if((table->count>>HASH_GROW_SHIFT) >= table->head_mask){
		int new_size = table->heads ? (table->head_mask+1) * 2 : 4;
		int alloc_size = sizeof(Property*)*new_size;
		Property ** new_heads = (Property**)malloc(alloc_size OS_DBG_FILEPOS);
		OS_ASSERT(new_heads);
		OS_MEMSET(new_heads, 0, alloc_size);

		Property ** old_heads = table->heads;
		table->heads = new_heads;
		table->head_mask = new_size-1;

		for(Property * cur = table->first; cur; cur = cur->next){
			int slot = cur->getHash() & table->head_mask;
			cur->hash_next = table->heads[slot];
			table->heads[slot] = cur;
		}

		// delete [] old_heads;
		free(old_heads);
	}

	int slot = prop->getHash() & table->head_mask;
	prop->hash_next = table->heads[slot];
	table->heads[slot] = prop;

	if(!table->first){
		table->first = prop;    
	}else{
		OS_ASSERT(table->last);
		table->last->next = prop;
		prop->prev = table->last;
	}
	table->last = prop;

	if(prop->index.type == OS_VALUE_TYPE_NUMBER && table->next_index <= prop->index.v.number){
		table->next_index = (OS_INT)prop->index.v.number + 1;
	}

	table->count++;
}

void OS::Core::changePropertyIndex(Table * table, Property * prop, const PropertyIndex& new_index)
{
	int slot = prop->getHash() & table->head_mask;
	Property * cur = table->heads[slot], * chain_prev = NULL;
	for(; cur; chain_prev = cur, cur = cur->hash_next){
		if(cur == prop){ // cur->isEqual(index)){
			if(chain_prev){
				chain_prev->hash_next = cur->hash_next;
			}else{
				table->heads[slot] = cur->hash_next;
			}
			break;
		}
	}
	OS_ASSERT(cur && cur == prop);
	if(cur){
		*prop = new_index;

		slot = prop->getHash() & table->head_mask;
		prop->hash_next = table->heads[slot];
		table->heads[slot] = prop;

		if(prop->index.type == OS_VALUE_TYPE_NUMBER && table->next_index <= prop->index.v.number){
			table->next_index = (OS_INT)prop->index.v.number + 1;
		}
	}
}

OS::Core::Property * OS::Core::removeTableProperty(Table * table, const PropertyIndex& index)
{
	OS_ASSERT(table);
	int slot = index.getHash() & table->head_mask;
	Property * cur = table->heads[slot], * chain_prev = NULL;
	for(; cur; chain_prev = cur, cur = cur->hash_next){
		if(cur->isEqual(index)){
			if(table->first == cur){
				table->first = cur->next;
				if(table->first){
					table->first->prev = NULL;
				}
			}else{
				OS_ASSERT(cur->prev);
				cur->prev->next = cur->next;
			}

			if(table->last == cur){
				table->last = cur->prev;
				if(table->last){
					table->last->next = NULL;
				}
			}else{
				OS_ASSERT(cur->next);
				cur->next->prev = cur->prev;
			}

			if(chain_prev){
				chain_prev->hash_next = cur->hash_next;
			}else{
				table->heads[slot] = cur->hash_next;
			}

			for(Table::IteratorState * iter = table->iterators; iter; iter = iter->next){
				if(iter->prop == cur){
					iter->prop = iter->ascending ? cur->next : cur->prev;
				}
			}

			cur->next = NULL;
			cur->prev = NULL;
			cur->hash_next = NULL;
			// cur->value.clear();

			table->count--;

			return cur;
		}
	}  
	return NULL;
}

bool OS::Core::deleteTableProperty(Table * table, const PropertyIndex& index)
{
	Property * prop = removeTableProperty(table, index);
	if(prop){
		prop->~Property();
		free(prop);
		return true;
	}
	return false;
}

void OS::Core::deleteValueProperty(GCValue * table_value, const PropertyIndex& index, bool anonymous_del_enabled, bool named_del_enabled, bool prototype_enabled)
{
	Table * table = table_value->table;
	if(table && deleteTableProperty(table, index)){
		return;
	}
	if(1){ // prototype_enabled){
		GCValue * cur_value = table_value;
		while(cur_value->prototype){
			cur_value = cur_value->prototype;
			Table * cur_table = cur_value->table;
			if(!cur_table){
				continue;
			}
			if(prototype_enabled){
				if(cur_table && deleteTableProperty(cur_table, index)){
					return;
				}
			}else{
				if(cur_table && cur_table->get(index)){
					return;
				}
			}
		}
	}
	if(index.index.type == OS_VALUE_TYPE_STRING && strings->syntax_prototype == index.index.v.string){
		return;
	}
	if(table_value->type == OS_VALUE_TYPE_ARRAY){
		OS_ASSERT(dynamic_cast<GCArrayValue*>(table_value));
		GCArrayValue * arr = (GCArrayValue*)table_value;
		int i = (int)valueToInt(index.index);
		if(i >= 0 && i < arr->values.count){
			allocator->vectorRemoveAtIndex(arr->values, i);
		}
		return;
	}
	if((anonymous_del_enabled || named_del_enabled) && !hasSpecialPrefix(index.index)){
		Value value;
		if(index.index.type == OS_VALUE_TYPE_STRING && named_del_enabled){
			const void * buf1 = strings->__delAt.toChar();
			int size1 = strings->__delAt.getDataSize();
			const void * buf2 = index.index.v.string->toChar();
			int size2 = index.index.v.string->getDataSize();
			GCStringValue * del_name = newStringValue(buf1, size1, buf2, size2);
			if(getPropertyValue(value, table_value, PropertyIndex(del_name, PropertyIndex::KeepStringIndex()), prototype_enabled)
				&& value.isFunction())
			{
				pushValue(value);
				pushValue(table_value);
				pushValue(index.index);
				call(1, 0);
				return;
			}
		}
		if(anonymous_del_enabled && getPropertyValue(value, table_value, PropertyIndex(strings->__del, PropertyIndex::KeepStringIndex()), prototype_enabled)
			&& value.isFunction())
		{
			pushValue(value);
			pushValue(table_value);
			pushValue(index.index);
			call(1, 0);
		}
	}
}

void OS::Core::deleteValueProperty(Value table_value, const PropertyIndex& index, bool anonymous_del_enabled, bool named_del_enabled, bool prototype_enabled)
{
	switch(table_value.type){
	case OS_VALUE_TYPE_NULL:
		return;

	case OS_VALUE_TYPE_BOOL:
		if(prototype_enabled){
			return deleteValueProperty(prototypes[PROTOTYPE_BOOL], index, anonymous_del_enabled, named_del_enabled, prototype_enabled);
		}
		return;

	case OS_VALUE_TYPE_NUMBER:
		if(prototype_enabled){
			return deleteValueProperty(prototypes[PROTOTYPE_NUMBER], index, anonymous_del_enabled, named_del_enabled, prototype_enabled);
		}
		return;

	case OS_VALUE_TYPE_STRING:
		/* if(prototype_enabled){
			return deleteValueProperty(prototypes[PROTOTYPE_STRING], index, anonymous_del_enabled, named_del_enabled, prototype_enabled);
		}
		return; */

	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		return deleteValueProperty(table_value.v.value, index, anonymous_del_enabled, named_del_enabled, prototype_enabled);
	}
}

void OS::Core::copyTableProperties(Table * dst, Table * src)
{
	// OS_ASSERT(dst->count == 0);
	for(Property * prop = src->first; prop; prop = prop->next){
		setTableValue(dst, PropertyIndex(*prop), prop->value);
	}
}

void OS::Core::copyTableProperties(GCValue * dst_value, GCValue * src_value, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	if(src_value->table){
		for(Property * prop = src_value->table->first; prop; prop = prop->next){
			setPropertyValue(dst_value, *prop, prop->value, anonymous_setter_enabled, named_setter_enabled);
		}
	}
}

void OS::Core::sortTable(Table * table, int(*comp)(OS*, const void*, const void*, void*), void * user_param, bool reorder_keys)
{
	if(table->count > 1){
		Property ** props = (Property**)malloc(sizeof(Property*) * table->count OS_DBG_FILEPOS);
		int i = 0;
		Property * cur = table->first;
		for(; cur && i < table->count; cur = cur->next, i++){
			props[i] = cur;
		}
		OS_ASSERT(!cur && i == table->count);
		allocator->qsort(props, table->count, sizeof(Core::Property*), comp, user_param);
		table->first = props[0];
		props[0]->prev = NULL;
		for(i = 1; i < table->count; i++){
			props[i-1]->next = props[i];
			props[i]->prev = props[i-1];
		}
		props[i-1]->next = NULL;
		table->last = props[i-1];

		if(reorder_keys){
#if 1 // speed optimization
			OS_MEMSET(table->heads, 0, sizeof(Property*)*(table->head_mask+1));
			for(i = 0; i < table->count; i++){
				Property * cur = props[i];
				cur->index = Value(i);
				int slot = cur->getHash() & table->head_mask;
				cur->hash_next = table->heads[slot];
				table->heads[slot] = cur;
			}
#else
			for(i = 0; i < table->count; i++){
				changePropertyIndex(table, props[i], Value(i));
			}
#endif
			table->next_index = table->count;
		}

		free(props);
	}
}

void OS::Core::sortArray(GCArrayValue * arr, int(*comp)(OS*, const void*, const void*, void*), void * user_param)
{
	allocator->qsort(arr->values.buf, arr->values.count, sizeof(Value), comp, user_param);
}

static int compareResult(OS_NUMBER num)
{
	if(num < 0) return -1;
	if(num > 0) return 1;
	return 0;
}

int OS::Core::comparePropValues(OS * os, const void * a, const void * b, void*)
{
	Property * props[] = {*(Property**)a, *(Property**)b};
	os->core->pushOpResultValue(Program::OP_COMPARE, props[0]->value, props[1]->value);
	return compareResult(os->popNumber());
}

int OS::Core::comparePropValuesReverse(OS * os, const void * a, const void * b, void * user_param)
{
	return comparePropValues(os, b, a, user_param);
}

int OS::Core::compareObjectProperties(OS * os, const void * a, const void * b, void * user_param)
{
	Property * props[] = {*(Property**)a, *(Property**)b};
	const String& name = *(String*)user_param;

	os->core->pushValue(props[0]->value);
	os->core->pushStringValue(name);
	os->getProperty();

	os->core->pushValue(props[1]->value);
	os->core->pushStringValue(name);
	os->getProperty();

	os->runOp(OP_COMPARE);
	return compareResult(os->popNumber());
}

int OS::Core::compareObjectPropertiesReverse(OS * os, const void * a, const void * b, void * user_param)
{
	return compareObjectProperties(os, b, a, user_param);
}

int OS::Core::compareUserPropValues(OS * os, const void * a, const void * b, void*)
{
	Property * props[] = {*(Property**)a, *(Property**)b};
	os->pushStackValue(-1);
	os->pushNull();
	os->core->pushValue(props[0]->value);
	os->core->pushValue(props[1]->value);
	os->core->pushValue(props[0]->index);
	os->core->pushValue(props[1]->index);
	os->call(4, 1);
	return compareResult(os->popNumber());
}

int OS::Core::compareUserPropValuesReverse(OS * os, const void * a, const void * b, void * user_param)
{
	return compareUserPropValues(os, b, a, user_param);
}

int OS::Core::comparePropKeys(OS * os, const void * a, const void * b, void*)
{
	Property * props[] = {*(Property**)a, *(Property**)b};
	os->core->pushOpResultValue(Program::OP_COMPARE, props[0]->index, props[1]->index);
	return compareResult(os->popNumber());
}

int OS::Core::comparePropKeysReverse(OS * os, const void * a, const void * b, void * user_param)
{
	return comparePropKeys(os, b, a, user_param);
}

int OS::Core::compareUserPropKeys(OS * os, const void * a, const void * b, void*)
{
	Property * props[] = {*(Property**)a, *(Property**)b};
	os->pushStackValue(-1);
	os->pushNull();
	os->core->pushValue(props[0]->index);
	os->core->pushValue(props[1]->index);
	os->core->pushValue(props[0]->value);
	os->core->pushValue(props[1]->value);
	os->call(4, 1);
	return compareResult(os->popNumber());
}

int OS::Core::compareUserPropKeysReverse(OS * os, const void * a, const void * b, void * user_param)
{
	return compareUserPropKeys(os, b, a, user_param);
}

int OS::Core::compareArrayValues(OS * os, const void * a, const void * b, void*)
{
	Value * values[] = {(Value*)a, (Value*)b};
	os->core->pushOpResultValue(Program::OP_COMPARE, *values[0], *values[1]);
	return compareResult(os->popNumber());
}

int OS::Core::compareArrayValuesReverse(OS * os, const void * a, const void * b, void * user_param)
{
	return compareArrayValues(os, b, a, user_param);
}

int OS::Core::compareUserArrayValues(OS * os, const void * a, const void * b, void*)
{
	Value * values[] = {(Value*)a, (Value*)b};
	os->pushStackValue(-1);
	os->pushNull();
	os->core->pushValue(*values[0]);
	os->core->pushValue(*values[1]);
	os->call(2, 1);
	return compareResult(os->popNumber());
}

int OS::Core::compareUserArrayValuesReverse(OS * os, const void * a, const void * b, void * user_param)
{
	return compareUserArrayValues(os, b, a, user_param);
}

int OS::Core::compareUserReverse(OS * os, const void * a, const void * b, void * user_param)
{
	int (*comp)(OS*, const void*, const void*, void*) = (int(*)(OS*, const void*, const void*, void*))user_param;
	return comp(os, b, a, NULL);
}

OS::Core::Property * OS::Core::Table::get(const PropertyIndex& index)
{
	if(heads){
		Property * cur = heads[index.getHash() & head_mask];
		for(; cur; cur = cur->hash_next){
			if(cur->isEqual(index)){
				return cur;
			}
		}
	}
	return NULL;
}

// =====================================================================

OS::Core::GCFunctionValue::GCFunctionValue()
{
	/*
	prog = NULL;
	func_decl = NULL;
	env = NULL;
	upvalues = NULL;
	*/
}

OS::Core::GCFunctionValue::~GCFunctionValue()
{
	OS_ASSERT(!upvalues && !name);
	OS_ASSERT(!prog && !func_decl);
}

OS::Core::GCFunctionValue * OS::Core::newFunctionValue(StackFunction * stack_func, Program * prog, FunctionDecl * func_decl, Value env)
{
	GCFunctionValue * func_value = new (allocator->malloc(sizeof(GCFunctionValue) OS_DBG_FILEPOS)) GCFunctionValue();
	func_value->type = OS_VALUE_TYPE_FUNCTION;
	func_value->prototype = prototypes[PROTOTYPE_FUNCTION];
	func_value->prog = prog->retain();
	func_value->func_decl = func_decl;
	func_value->env = env; // global_vars;
	func_value->upvalues = stack_func ? stack_func->locals->retain() : NULL;
	func_value->name = NULL;
	registerValue(func_value);
	// pushValue(func_value);
	return func_value;
}

void OS::Core::clearFunctionValue(GCFunctionValue * func_value)
{
	OS_ASSERT(func_value->prog && func_value->func_decl); // && func_data->env); //  && func_data->self

	// value could be already destroyed by gc or will be destroyed soon
	// releaseValue(func_data->env);
	func_value->env = (GCValue*)NULL;

	// releaseValue(func_data->self);
	// func_data->self = NULL;
	OS_ASSERT(func_value->func_decl);

	if(func_value->upvalues){
		releaseUpvalues(func_value->upvalues);
		func_value->upvalues = NULL;
	}
	func_value->name = NULL;

	func_value->func_decl = NULL;

	func_value->prog->release();
	func_value->prog = NULL;

	// func_value->~GCFunctionValue();
	// free(func_value);
}

// =====================================================================
/*
OS::Core::Upvalues::Upvalues()
{
ref_count = 1;
gc_time = -1;

locals = NULL;
is_stack_locals = false;

num_locals = 0;
num_params = 0;
num_extra_params = 0;

arguments = NULL;
rest_arguments = NULL;

num_parents = 0;
}

OS::Core::Upvalues::~Upvalues()
{
OS_ASSERT(!ref_count);
OS_ASSERT(!locals && !arguments && !rest_arguments);
}
*/

OS::Core::Upvalues ** OS::Core::Upvalues::getParents()
{
	return (Upvalues**)(this + 1);
}

OS::Core::Upvalues * OS::Core::Upvalues::getParent(int i)
{
	OS_ASSERT(i >= 0 && i < num_parents);
	return ((Upvalues**)(this+1))[i];
}

OS::Core::Upvalues * OS::Core::Upvalues::retain()
{
	ref_count++;
	return this;
}

// =====================================================================
#if 0
OS::Core::StackFunction::StackFunction()
{
	/*
	func = NULL;
	self = NULL;
	locals = NULL;

	caller_stack_pos = 0;
	locals_stack_pos = 0;
	opcode_stack_pos = 0;
	bottom_stack_pos = 0;

	need_ret_values = 0;
	opcodes_offs = 0;
	*/
}

OS::Core::StackFunction::~StackFunction()
{
	OS_ASSERT(!func && !self && !locals);
}
#endif

// =====================================================================

OS::Core::Value::Value()
{
	v.value = NULL;
	type = OS_VALUE_TYPE_NULL;
}

OS::Core::Value::Value(bool val)
{
	v.boolean = val;
	type = OS_VALUE_TYPE_BOOL;
}

OS::Core::Value::Value(OS_INT32 val)
{
	v.number = (OS_NUMBER)val;
	type = OS_VALUE_TYPE_NUMBER;
}

OS::Core::Value::Value(OS_INT64 val)
{
	v.number = (OS_NUMBER)val;
	type = OS_VALUE_TYPE_NUMBER;
}

OS::Core::Value::Value(float val)
{
	v.number = (OS_NUMBER)val;
	type = OS_VALUE_TYPE_NUMBER;
}

OS::Core::Value::Value(double val)
{
	v.number = (OS_NUMBER)val;
	type = OS_VALUE_TYPE_NUMBER;
}

OS::Core::Value::Value(int val, const WeakRef&)
{
	v.value_id = val;
	type = OS_VALUE_TYPE_WEAKREF;
}

OS::Core::Value::Value(GCValue * val)
{
	if(val){
		v.value = val;
		type = val->type;
	}else{
		v.value = NULL;
		type = OS_VALUE_TYPE_NULL;
	}
}

OS::Core::Value& OS::Core::Value::operator=(GCValue * val)
{
	if(val){
		v.value = val;
		type = val->type;
	}else{
		v.value = NULL;
		type = OS_VALUE_TYPE_NULL;
	}
	return *this;
}

OS::Core::Value& OS::Core::Value::operator=(bool val)
{
	v.boolean = val;
	type = OS_VALUE_TYPE_BOOL;
	return *this;
}

OS::Core::Value& OS::Core::Value::operator=(OS_INT32 val)
{
	v.number = (OS_NUMBER)val;
	type = OS_VALUE_TYPE_NUMBER;
	return *this;
}

OS::Core::Value& OS::Core::Value::operator=(OS_INT64 val)
{
	v.number = (OS_NUMBER)val;
	type = OS_VALUE_TYPE_NUMBER;
	return *this;
}

OS::Core::Value& OS::Core::Value::operator=(float val)
{
	v.number = (OS_NUMBER)val;
	type = OS_VALUE_TYPE_NUMBER;
	return *this;
}

OS::Core::Value& OS::Core::Value::operator=(double val)
{
	v.number = (OS_NUMBER)val;
	type = OS_VALUE_TYPE_NUMBER;
	return *this;
}

void OS::Core::Value::clear()
{
	v.value = NULL;
	type = OS_VALUE_TYPE_NULL;
}

OS::Core::GCValue * OS::Core::Value::getGCValue() const
{
	switch(type){
	case OS_VALUE_TYPE_STRING:
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		OS_ASSERT(v.value);
		return v.value;
	}
	return NULL;
}

bool OS::Core::Value::isNull() const
{
	return type == OS_VALUE_TYPE_NULL;
}

bool OS::Core::Value::isFunction() const
{
	switch(type){
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		return true;
	}
	return false;
}

bool OS::Core::Value::isUserdata() const
{
	switch(type){
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		return true;
	}
	return false;
}

// =====================================================================

OS::Core::ValueRetained::ValueRetained(): super()
{
}

OS::Core::ValueRetained::ValueRetained(bool val): super(val)
{
	// retain();
}

OS::Core::ValueRetained::ValueRetained(OS_FLOAT val): super(val)
{
	// retain();
}

OS::Core::ValueRetained::ValueRetained(int val): super(val)
{
	// retain();
}

OS::Core::ValueRetained::ValueRetained(int val, const WeakRef& wr): super(val, wr)
{
	// retain();
}

OS::Core::ValueRetained::ValueRetained(GCValue * val): super(val)
{
	// retain();
	if(val){
		val->external_ref_count++;
	}
}

OS::Core::ValueRetained::ValueRetained(Value b): super(b)
{
	retain();
}

OS::Core::ValueRetained::~ValueRetained()
{
	release();
}

OS::Core::ValueRetained& OS::Core::ValueRetained::operator=(Value b)
{
	release();
	super::operator=(b);
	retain();
	return *this;
}


void OS::Core::ValueRetained::clear()
{
	release();
	super::clear();
}

void OS::Core::ValueRetained::retain()
{
	switch(type){
	case OS_VALUE_TYPE_STRING:
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		OS_ASSERT(v.value);
		v.value->external_ref_count++;
		break;
	}
}

void OS::Core::ValueRetained::release()
{
	switch(type){
	case OS_VALUE_TYPE_STRING:
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		OS_ASSERT(v.value && v.value->external_ref_count > 0);
		v.value->external_ref_count--;
		if(v.value->gc_color == GC_WHITE){
			v.value->gc_color = GC_BLACK;
		}
		break;
	}
}

// =====================================================================

OS::Core::GCValue::GCValue()
{
	value_id = 0;
	external_ref_count = 0;
	hash_next = NULL;
	prototype = NULL;
	table = NULL;
	gc_grey_next = NULL;
#ifdef OS_DEBUG
	gc_time = -1;
#endif
	gc_color = GC_WHITE;
	type = OS_VALUE_TYPE_NULL;
	is_object_instance = false;
}

OS::Core::GCValue::~GCValue()
{
	value_id = 0;

	OS_ASSERT(type == OS_VALUE_TYPE_UNKNOWN);
	OS_ASSERT(!table);
	OS_ASSERT(!hash_next);
	OS_ASSERT(!prototype);
	OS_ASSERT(gc_color != GC_GREY);
}

// =====================================================================

OS::Core::GCStringValue::GCStringValue(int p_data_size)
{
	data_size = p_data_size;
}

OS::Core::GCStringValue * OS::Core::GCStringValue::alloc(OS * allocator, const void * buf, int data_size OS_DBG_FILEPOS_DECL)
{
	OS_ASSERT(data_size >= 0);
	int alloc_size = data_size + sizeof(GCStringValue) + sizeof(wchar_t) + sizeof(wchar_t)/2;
	GCStringValue * string = new (allocator->malloc(alloc_size OS_DBG_FILEPOS_PARAM)) GCStringValue(data_size);
	string->type = OS_VALUE_TYPE_STRING;
	string->prototype = allocator->core->prototypes[PROTOTYPE_STRING];
	OS_BYTE * data_buf = string->toBytes();
	OS_MEMCPY(data_buf, buf, data_size);
	OS_MEMSET(data_buf + data_size, 0, sizeof(wchar_t) + sizeof(wchar_t)/2);
	string->calcHash();
	allocator->core->registerValue(string);
#ifdef OS_DEBUG
	string->str = string->toChar();
#endif
	return string;
}

OS::Core::GCStringValue * OS::Core::GCStringValue::alloc(OS * allocator, const void * buf1, int len1, const void * buf2, int len2 OS_DBG_FILEPOS_DECL)
{
	OS_ASSERT(len1 >= 0 && len2 >= 0);
	int alloc_size = len1 + len2 + sizeof(GCStringValue) + sizeof(wchar_t) + sizeof(wchar_t)/2;
	GCStringValue * string = new (allocator->malloc(alloc_size OS_DBG_FILEPOS_PARAM)) GCStringValue(len1 + len2);
	string->type = OS_VALUE_TYPE_STRING;
	string->prototype = allocator->core->prototypes[PROTOTYPE_STRING];
	OS_BYTE * data_buf = string->toBytes();
	OS_MEMCPY(data_buf, buf1, len1); data_buf += len1;
	if(len2){ OS_MEMCPY(data_buf, buf2, len2); data_buf += len2; }
	OS_MEMSET(data_buf, 0, sizeof(wchar_t) + sizeof(wchar_t)/2);
	string->calcHash();
	allocator->core->registerValue(string);
#ifdef OS_DEBUG
	string->str = string->toChar();
#endif
	return string;
}

/*
OS::Core::GCStringValue * OS::Core::GCStringValue::alloc(OS * allocator, const void * buf1, int len1, 
const void * buf2, int len2, const void * buf3, int len3)
{
OS_ASSERT(len1 >= 0 && len2 >= 0 && len3 >= 0);
int alloc_size = len1 + len2 + len3 + sizeof(GCStringValue) + sizeof(wchar_t) + sizeof(wchar_t)/2;
GCStringValue * string = new (allocator->malloc(alloc_size)) GCStringValue(len1 + len2 + len3);
string->type = OS_VALUE_TYPE_STRING;
string->prototype = allocator->core->prototypes[PROTOTYPE_STRING];
OS_BYTE * data_buf = string->toBytes();
OS_MEMCPY(data_buf, buf1, len1); data_buf += len1;
OS_MEMCPY(data_buf, buf2, len2); data_buf += len2;
OS_MEMCPY(data_buf, buf3, len3); data_buf += len3;
OS_MEMSET(data_buf, 0, sizeof(wchar_t) + sizeof(wchar_t)/2);
string->calcHash();
allocator->core->registerValue(string);
return string;
}
*/

OS::Core::GCStringValue * OS::Core::GCStringValue::alloc(OS * allocator, GCStringValue * a, GCStringValue * b OS_DBG_FILEPOS_DECL)
{
	return alloc(allocator, a->toMemory(), a->data_size, b->toMemory(), b->data_size OS_DBG_FILEPOS_PARAM);
}

/*
OS::Core::GCStringValue * OS::Core::GCStringValue::alloc(OS * allocator, GCStringValue * a, GCStringValue * b, GCStringValue * c)
{
return alloc(allocator, a->toMemory(), a->data_size, b->toMemory(), b->data_size, c->toMemory(), c->data_size);
}
*/

bool OS::Core::GCStringValue::isNumber(OS_NUMBER* p_val) const
{
	const OS_CHAR * str = toChar();
	const OS_CHAR * end = str + getLen();
	OS_FLOAT val;
	if(Utils::parseFloat(str, val) && (str == end || (*str==OS_TEXT('f') && str+1 == end))){
		if(p_val) *p_val = (OS_NUMBER)val;
		return true;
	}
	if(p_val) *p_val = 0;
	return false;
}

OS_NUMBER OS::Core::GCStringValue::toNumber() const
{
	const OS_CHAR * str = toChar();
	const OS_CHAR * end = str + getLen();
	OS_FLOAT val;
	if(Utils::parseFloat(str, val) && (str == end || (*str==OS_TEXT('f') && str+1 == end))){
		return (OS_NUMBER)val;
	}
	return 0;
}

int OS::Core::GCStringValue::cmp(GCStringValue * string) const
{
	if(this == string){
		return 0;
	}
	return Utils::cmp(toBytes(), data_size, string->toBytes(), string->data_size);
}

int OS::Core::GCStringValue::cmp(const OS_CHAR * str) const
{
	return cmp(str, OS_STRLEN(str));
}

int OS::Core::GCStringValue::cmp(const OS_CHAR * str, int len) const
{
	return Utils::cmp(toBytes(), data_size, str, len * sizeof(OS_CHAR));
}

void OS::Core::GCStringValue::calcHash()
{
	hash = Utils::keyToHash(toBytes(), data_size); 
}

// =====================================================================

bool OS::Core::valueToBool(const Value& val)
{
	switch(val.type){
	case OS_VALUE_TYPE_NULL:
		return false;

	case OS_VALUE_TYPE_BOOL:
		return val.v.boolean ? true : false;

	case OS_VALUE_TYPE_NUMBER:
		// return val->value.number && !OS_ISNAN(val->value.number);
		return !OS_ISNAN((OS_FLOAT)val.v.number);

		// case OS_VALUE_TYPE_STRING:
		//	return val->value.string_data->data_size > 0;

		// case OS_VALUE_TYPE_OBJECT:
		// case OS_VALUE_TYPE_ARRAY:
		// 	return val->table ? val->table->count : 0;
	}
	return true;
}

OS_INT OS::Core::valueToInt(const Value& val, bool valueof_enabled)
{
	return (OS_INT)valueToNumber(val, valueof_enabled);
}

OS_INT OS::Core::Compiler::Expression::toInt()
{
	return (OS_INT)toNumber();
}

OS_NUMBER OS::Core::Compiler::Expression::toNumber()
{
	switch(type){
	case EXP_TYPE_CONST_NULL:
		return 0;

	case EXP_TYPE_CONST_STRING:
		return token->str.toNumber();

	case EXP_TYPE_CONST_NUMBER:
		return (OS_NUMBER)token->getFloat();

	case EXP_TYPE_CONST_TRUE:
		return 1;

	case EXP_TYPE_CONST_FALSE:
		return 0;
	}
	OS_ASSERT(false);
	return 0;
}

OS_NUMBER OS::Core::valueToNumber(const Value& val, bool valueof_enabled)
{
	switch(val.type){
	case OS_VALUE_TYPE_NULL:
		return 0; // nan_float;

	case OS_VALUE_TYPE_BOOL:
		return (OS_NUMBER)val.v.boolean;

	case OS_VALUE_TYPE_NUMBER:
		return val.v.number;

	case OS_VALUE_TYPE_STRING:
		return val.v.string->toNumber();
	}
	if(valueof_enabled){
		pushValueOf(val);
		struct Pop { Core * core; ~Pop(){ core->pop(); } } pop = {this};
		return valueToNumber(stack_values.lastElement(), false);
	}
	return 0;
}

bool OS::Core::isValueNumber(Value val, OS_NUMBER * out)
{
	switch(val.type){
		/*
		case OS_VALUE_TYPE_NULL:
		if(out){
		*out = 0;
		}
		return true;
		*/

	case OS_VALUE_TYPE_BOOL:
		if(out){
			*out = (OS_NUMBER)val.v.boolean;
		}
		return true;

	case OS_VALUE_TYPE_NUMBER:
		if(out){
			*out = (OS_NUMBER)val.v.number;
		}
		return true;

	case OS_VALUE_TYPE_STRING:
		return val.v.string->isNumber(out);
	}
	if(out){
		*out = 0;
	}
	return false;
}

OS::Core::String OS::Core::Compiler::Expression::toString()
{
	switch(type){
	case EXP_TYPE_CONST_NULL:
		// return String(getAllocator());
		return String(getAllocator(), OS_TEXT("null"));

	case EXP_TYPE_CONST_STRING:
		return token->str;

	case EXP_TYPE_CONST_NUMBER:
		// OS_ASSERT(token->str.toFloat() == token->getFloat());
		// return token->str;
		return String(getAllocator(), token->getFloat());

	case EXP_TYPE_CONST_TRUE:
		// return String(getAllocator(), OS_TEXT("1"));
		return String(getAllocator(), OS_TEXT("true"));

	case EXP_TYPE_CONST_FALSE:
		// return String(getAllocator());
		return String(getAllocator(), OS_TEXT("false"));
	}
	OS_ASSERT(false);
	return String(getAllocator());
}

OS::Core::String OS::Core::valueToString(Value val, bool valueof_enabled)
{
	switch(val.type){
	case OS_VALUE_TYPE_NULL:
		// return String(allocator);
		return strings->syntax_null;

	case OS_VALUE_TYPE_BOOL:
		// return val->value.boolean ? String(allocator, OS_TEXT("1")) : String(allocator);
		return val.v.boolean ? strings->syntax_true : strings->syntax_false;

	case OS_VALUE_TYPE_NUMBER:
		return String(allocator, val.v.number, OS_AUTO_PRECISION);

	case OS_VALUE_TYPE_STRING:
		return String(val.v.string);
	}
	if(valueof_enabled){
		/*
		Value * func = getPropertyValue(val, PropertyIndex(strings->__tostring, PropertyIndex::KeepStringIndex()), prototype_enabled);
		if(func){
		pushValue(func);
		pushValue(val);
		call(0, 1);
		OS_ASSERT(stack_values.count > 0);
		struct Pop { Core * core; ~Pop(){ core->pop(); } } pop = {this};
		return valueToString(stack_values.lastElement(), false);
		}
		*/
		pushValueOf(val);
		struct Pop { Core * core; ~Pop(){ core->pop(); } } pop = {this};
		return valueToString(stack_values.lastElement(), false);
	}
	return String(allocator);
}

bool OS::Core::isValueString(Value val, String * out)
{
	switch(val.type){
	case OS_VALUE_TYPE_NULL:
		if(out){
			// *out = String(allocator);
			*out = strings->syntax_null;
		}
		return false;

	case OS_VALUE_TYPE_BOOL:
		if(out){
			// *out = String(allocator, val->value.boolean ? OS_TEXT("1") : OS_TEXT(""));
			*out = val.v.boolean ? strings->syntax_true : strings->syntax_false;
		}
		return true;

	case OS_VALUE_TYPE_NUMBER:
		if(out){
			*out = String(allocator, val.v.number, OS_AUTO_PRECISION);
		}
		return true;

	case OS_VALUE_TYPE_STRING:
		if(out){
			*out = String(val.v.string);
		}
		return true;

		// case OS_VALUE_TYPE_OBJECT:
		// case OS_VALUE_TYPE_ARRAY:
		// 	return String(this, (OS_INT)(val->table ? val->table->count : 0));
	}
	if(out){
		*out = String(allocator);
	}
	return false;
}

bool OS::Core::isValueString(Value val, OS::String * out)
{
	switch(val.type){
	case OS_VALUE_TYPE_NULL:
		if(out){
			// *out = String(allocator);
			*out = strings->syntax_null;
		}
		return false;

	case OS_VALUE_TYPE_BOOL:
		if(out){
			// *out = String(allocator, val->value.boolean ? OS_TEXT("1") : OS_TEXT(""));
			*out = val.v.boolean ? strings->syntax_true : strings->syntax_false;
		}
		return true;

	case OS_VALUE_TYPE_NUMBER:
		if(out){
			*out = String(allocator, val.v.number, OS_AUTO_PRECISION);
		}
		return true;

	case OS_VALUE_TYPE_STRING:
		if(out){
			*out = String(val.v.string);
		}
		return true;

		// case OS_VALUE_TYPE_OBJECT:
		// case OS_VALUE_TYPE_ARRAY:
		// 	return String(this, (OS_INT)(val->table ? val->table->count : 0));
	}
	if(out){
		*out = String(allocator);
	}
	return false;
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::Core::StringRefs::StringRefs()
{
	head_mask = 0;
	heads = NULL;
	count = 0;
}
OS::Core::StringRefs::~StringRefs()
{
	OS_ASSERT(count == 0);
	OS_ASSERT(!heads);
}

void OS::Core::registerStringRef(StringRef * str_ref)
{
	if((string_refs.count>>HASH_GROW_SHIFT) >= string_refs.head_mask){
		int new_size = string_refs.heads ? (string_refs.head_mask+1) * 2 : 32;
		int alloc_size = sizeof(StringRef*) * new_size;
		StringRef ** new_heads = (StringRef**)malloc(alloc_size OS_DBG_FILEPOS);
		OS_ASSERT(new_heads);
		OS_MEMSET(new_heads, 0, alloc_size);

		StringRef ** old_heads = string_refs.heads;
		int old_mask = string_refs.head_mask;

		string_refs.heads = new_heads;
		string_refs.head_mask = new_size-1;

		if(old_heads){
			for(int i = 0; i <= old_mask; i++){
				for(StringRef * str_ref = old_heads[i], * next; str_ref; str_ref = next){
					next = str_ref->hash_next;
					int slot = str_ref->string_hash & string_refs.head_mask;
					str_ref->hash_next = string_refs.heads[slot];
					string_refs.heads[slot] = str_ref;
				}
			}
			free(old_heads);
		}
	}

	int slot = str_ref->string_hash & string_refs.head_mask;
	str_ref->hash_next = string_refs.heads[slot];
	string_refs.heads[slot] = str_ref;
	string_refs.count++;
}

void OS::Core::unregisterStringRef(StringRef * str_ref)
{
	int slot = str_ref->string_hash & string_refs.head_mask;
	StringRef * cur = string_refs.heads[slot], * prev = NULL;
	for(; cur; prev = cur, cur = cur->hash_next){
		if(cur == str_ref){
			if(prev){
				prev->hash_next = cur->hash_next;
			}else{
				string_refs.heads[slot] = cur->hash_next;
			}
			OS_ASSERT(string_refs.count > 0);
			string_refs.count--;
			cur->hash_next = NULL;
			return;
		}
	}
	OS_ASSERT(false);
}

void OS::Core::deleteStringRefs()
{
	if(!string_refs.heads){
		return;
	}
	for(int i = 0; i <= string_refs.head_mask; i++){
		while(string_refs.heads[i]){
			StringRef * cur = string_refs.heads[i];
			string_refs.heads[i] = cur->hash_next;
			free(cur);
		}
	}
	free(string_refs.heads);
	string_refs.heads = NULL;
	string_refs.head_mask = 0;
	string_refs.count = 0;
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::Core::UserptrRefs::UserptrRefs()
{
	head_mask = 0;
	heads = NULL;
	count = 0;
}
OS::Core::UserptrRefs::~UserptrRefs()
{
	OS_ASSERT(count == 0);
	OS_ASSERT(!heads);
}

void OS::Core::registerUserptrRef(UserptrRef * user_pointer_ref)
{
	if((userptr_refs.count>>HASH_GROW_SHIFT) >= userptr_refs.head_mask){
		int new_size = userptr_refs.heads ? (userptr_refs.head_mask+1) * 2 : 32;
		int alloc_size = sizeof(UserptrRef*) * new_size;
		UserptrRef ** new_heads = (UserptrRef**)malloc(alloc_size OS_DBG_FILEPOS);
		OS_ASSERT(new_heads);
		OS_MEMSET(new_heads, 0, alloc_size);

		UserptrRef ** old_heads = userptr_refs.heads;
		int old_mask = userptr_refs.head_mask;

		userptr_refs.heads = new_heads;
		userptr_refs.head_mask = new_size-1;

		if(old_heads){
			for(int i = 0; i <= old_mask; i++){
				for(UserptrRef * user_pointer_ref = old_heads[i], * next; user_pointer_ref; user_pointer_ref = next){
					next = user_pointer_ref->hash_next;
					int slot = user_pointer_ref->userptr_hash & userptr_refs.head_mask;
					user_pointer_ref->hash_next = userptr_refs.heads[slot];
					userptr_refs.heads[slot] = user_pointer_ref;
				}
			}
			free(old_heads);
		}
	}

	int slot = user_pointer_ref->userptr_hash & userptr_refs.head_mask;
	user_pointer_ref->hash_next = userptr_refs.heads[slot];
	userptr_refs.heads[slot] = user_pointer_ref;
	userptr_refs.count++;
}

void OS::Core::unregisterUserptrRef(UserptrRef * userptr_ref)
{
	int slot = userptr_ref->userptr_hash & userptr_refs.head_mask;
	UserptrRef * cur = userptr_refs.heads[slot], * prev = NULL;
	for(; cur; prev = cur, cur = cur->hash_next){
		if(cur == userptr_ref){
			if(prev){
				prev->hash_next = cur->hash_next;
			}else{
				userptr_refs.heads[slot] = cur->hash_next;
			}
			OS_ASSERT(userptr_refs.count > 0);
			userptr_refs.count--;
			cur->hash_next = NULL;
			return;
		}
	}
	OS_ASSERT(false);
}

void OS::Core::unregisterUserptrRef(void * ptr, int value_id)
{
	if(userptr_refs.count > 0){
		OS_ASSERT(userptr_refs.heads && userptr_refs.head_mask);
		int hash = (int)(intptr_t)ptr;
		int slot = hash & userptr_refs.head_mask;
		UserptrRef * userptr_ref = userptr_refs.heads[slot];
		for(UserptrRef * prev = NULL, * next; userptr_ref; userptr_ref = next){
			next = userptr_ref->hash_next;
			if(userptr_ref->userptr_value_id == value_id){
				if(!prev){
					userptr_refs.heads[slot] = next;
				}else{
					prev->hash_next = next;					
				}
				free(userptr_ref);
				userptr_refs.count--;
				return;
			}
		}
	}
}

void OS::Core::deleteUserptrRefs()
{
	if(!userptr_refs.heads){
		return;
	}
	for(int i = 0; i <= userptr_refs.head_mask; i++){
		while(userptr_refs.heads[i]){
			UserptrRef * cur = userptr_refs.heads[i];
			userptr_refs.heads[i] = cur->hash_next;
			free(cur);
		}
	}
	free(userptr_refs.heads);
	userptr_refs.heads = NULL;
	userptr_refs.head_mask = 0;
	userptr_refs.count = 0;
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::Core::Values::Values()
{
	head_mask = 0; // OS_DEF_VALUES_HASH_SIZE-1;
	heads = NULL; // new Value*[OS_DEF_VALUES_HASH_SIZE];
	// OS_ASSERT(heads);
	next_id = 1;
	count = 0;
}
OS::Core::Values::~Values()
{
	OS_ASSERT(count == 0);
	OS_ASSERT(!heads);
	// delete [] heads;
}

void OS::Core::registerValue(GCValue * value)
{
	value->value_id = values.next_id++;

	if((values.count>>HASH_GROW_SHIFT) >= values.head_mask){
		int new_size = values.heads ? (values.head_mask+1) * 2 : 32;
		int alloc_size = sizeof(GCValue*) * new_size;
		GCValue ** new_heads = (GCValue**)malloc(alloc_size OS_DBG_FILEPOS); // new Value*[new_size];
		OS_ASSERT(new_heads);
		OS_MEMSET(new_heads, 0, alloc_size);

		GCValue ** old_heads = values.heads;
		int old_mask = values.head_mask;

		values.heads = new_heads;
		values.head_mask = new_size-1;

		if(old_heads){
			for(int i = 0; i <= old_mask; i++){
				for(GCValue * value = old_heads[i], * next; value; value = next){
					gcAddToGreyList(value);
					next = value->hash_next;
					int slot = value->value_id & values.head_mask;
					value->hash_next = values.heads[slot];
					values.heads[slot] = value;
				}
			}
			// delete [] old_heads;
			free(old_heads);
		}
		if(gc_values_head_index >= 0){
			// restart gc ASAP
			gc_values_head_index = -1;
			gc_start_next_values = 0;
			gc_continuous = false;
			gc_step_size_auto_mult *= 4.0f;
		}
	}

	int slot = value->value_id & values.head_mask;
	value->hash_next = values.heads[slot];
	values.heads[slot] = value;
	values.count++;

	num_created_values++;

	gcAddToGreyList(value);
	// value->gc_color = GC_BLACK;

	gcStepIfNeeded();
}

OS::Core::GCValue * OS::Core::unregisterValue(int value_id)
{
	int slot = value_id & values.head_mask;
	GCValue * value = values.heads[slot], * prev = NULL;
	for(; value; prev = value, value = value->hash_next){
		if(value->value_id == value_id){
			if(prev){
				prev->hash_next = value->hash_next;
			}else{
				values.heads[slot] = value->hash_next;
			}
			OS_ASSERT(values.count > 0);
			values.count--;
			value->hash_next = NULL;
			return value;
		}
	}
	return NULL;
}

void OS::Core::deleteValues(bool del_ref_counted_also)
{
	if(values.heads && values.count > 0){
		while(true){
			for(int i = 0; i <= values.head_mask; i++){
#if 0
				for(GCValue * value; value = values.heads[i]; ){
					deleteValue(value);
				}
#else
				for(GCValue * value = values.heads[i], * next; value; value = next){
					next = value->hash_next;
					if(del_ref_counted_also || !value->external_ref_count){
						deleteValue(value);
					}
				}
#endif
			}
			if(!values.count || !del_ref_counted_also){
				break;
			}
		}
	}
	if(values.heads && values.count == 0){
		free(values.heads);
		values.heads = NULL;
		values.head_mask = 0;
		values.next_id = 1;
	}
}

OS::Core::GCValue * OS::Core::Values::get(int value_id)
{
	int slot = value_id & head_mask;
	for(GCValue * value = heads[slot]; value; value = value->hash_next){
		if(value->value_id == value_id){
			return value;
		}
	}
	return NULL;
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::Core::Strings::Strings(OS * allocator)
	:
	__construct(allocator, OS_TEXT("__construct")),
	// __destruct(allocator, OS_TEXT("__destruct")),
	__object(allocator, OS_TEXT("__object")),
	__get(allocator, OS_TEXT("__get")),
	__set(allocator, OS_TEXT("__set")),
	__getAt(allocator, OS_TEXT("__get@")),
	__setAt(allocator, OS_TEXT("__set@")),
	__del(allocator, OS_TEXT("__del")),
	__delAt(allocator, OS_TEXT("__del@")),
	__getempty(allocator, OS_TEXT("__getempty")),
	__setempty(allocator, OS_TEXT("__setempty")),
	__delempty(allocator, OS_TEXT("__delempty")),
	__getdim(allocator, OS_TEXT("__getdim")),
	__setdim(allocator, OS_TEXT("__setdim")),
	__deldim(allocator, OS_TEXT("__deldim")),
	__cmp(allocator, OS_TEXT("__cmp")),
	__iter(allocator, OS_TEXT("__iter")),
	// __tostring(allocator, OS_TEXT("__tostring")),
	__valueof(allocator, OS_TEXT("__valueof")),
	/*
	__booleanof(allocator, OS_TEXT("__booleanof")),
	__numberof(allocator, OS_TEXT("__numberof")),
	__stringof(allocator, OS_TEXT("__stringof")),
	__arrayof(allocator, OS_TEXT("__arrayof")),
	__objectof(allocator, OS_TEXT("__objectof")),
	__userdataof(allocator, OS_TEXT("__userdataof")),
	__functionof(allocator, OS_TEXT("__functionof")),
	*/
	__clone(allocator, OS_TEXT("__clone")),
	__concat(allocator, OS_TEXT("__concat")),
	__bitand(allocator, OS_TEXT("__bitand")),
	__bitor(allocator, OS_TEXT("__bitor")),
	__bitxor(allocator, OS_TEXT("__bitxor")),
	__bitnot(allocator, OS_TEXT("__bitnot")),
	__plus(allocator, OS_TEXT("__plus")),
	__neg(allocator, OS_TEXT("__neg")),
	__len(allocator, OS_TEXT("__len")),
	__add(allocator, OS_TEXT("__add")),
	__sub(allocator, OS_TEXT("__sub")),
	__mul(allocator, OS_TEXT("__mul")),
	__div(allocator, OS_TEXT("__div")),
	__mod(allocator, OS_TEXT("__mod")),
	__lshift(allocator, OS_TEXT("__lshift")),
	__rshift(allocator, OS_TEXT("__rshift")),
	__pow(allocator, OS_TEXT("__pow")),
	func_extends(allocator, OS_TEXT("extends")),
	func_in(allocator, OS_TEXT("__in")),
	func_is(allocator, OS_TEXT("__is")),
	func_isprototypeof(allocator, OS_TEXT("__isprototypeof")),
	func_push(allocator, OS_TEXT("push")),

	typeof_null(allocator, OS_TEXT("null")),
	typeof_boolean(allocator, OS_TEXT("boolean")),
	typeof_number(allocator, OS_TEXT("number")),
	typeof_string(allocator, OS_TEXT("string")),
	typeof_object(allocator, OS_TEXT("object")),
	typeof_array(allocator, OS_TEXT("array")),
	typeof_userdata(allocator, OS_TEXT("userdata")),
	typeof_function(allocator, OS_TEXT("function")),

	syntax_get(allocator, OS_TEXT("get")),
	syntax_set(allocator, OS_TEXT("set")),
	syntax_super(allocator, OS_TEXT("super")),
	syntax_is(allocator, OS_TEXT("is")),
	syntax_isprototypeof(allocator, OS_TEXT("isprototypeof")),
	/*
	syntax_typeof(allocator, OS_TEXT("typeof")),
	syntax_valueof(allocator, OS_TEXT("valueof")),
	syntax_booleanof(allocator, OS_TEXT("booleanof")),
	syntax_numberof(allocator, OS_TEXT("numberof")),
	syntax_stringof(allocator, OS_TEXT("stringof")),
	syntax_arrayof(allocator, OS_TEXT("arrayof")),
	syntax_objectof(allocator, OS_TEXT("objectof")),
	syntax_userdataof(allocator, OS_TEXT("userdataof")),
	syntax_functionof(allocator, OS_TEXT("functionof")),
	syntax_clone(allocator, OS_TEXT("clone")),
	*/
	syntax_extends(allocator, OS_TEXT("extends")),
	syntax_delete(allocator, OS_TEXT("delete")),
	syntax_prototype(allocator, OS_TEXT("prototype")),
	syntax_var(allocator, OS_TEXT("var")),
	syntax_this(allocator, OS_TEXT("this")),
	syntax_arguments(allocator, OS_TEXT("arguments")),
	syntax_function(allocator, OS_TEXT("function")),
	syntax_null(allocator, OS_TEXT("null")),
	syntax_true(allocator, OS_TEXT("true")),
	syntax_false(allocator, OS_TEXT("false")),
	syntax_return(allocator, OS_TEXT("return")),
	syntax_class(allocator, OS_TEXT("class")),
	syntax_enum(allocator, OS_TEXT("enum")),
	syntax_switch(allocator, OS_TEXT("switch")),
	syntax_case(allocator, OS_TEXT("case")),
	syntax_default(allocator, OS_TEXT("default")),
	syntax_if(allocator, OS_TEXT("if")),
	syntax_else(allocator, OS_TEXT("else")),
	syntax_elseif(allocator, OS_TEXT("elseif")),
	syntax_for(allocator, OS_TEXT("for")),
	syntax_in(allocator, OS_TEXT("in")),
	syntax_break(allocator, OS_TEXT("break")),
	syntax_continue(allocator, OS_TEXT("continue")),
	syntax_try(allocator, OS_TEXT("try")),
	syntax_catch(allocator, OS_TEXT("catch")),
	syntax_finally(allocator, OS_TEXT("finally")),
	syntax_throw(allocator, OS_TEXT("throw")),
	syntax_new(allocator, OS_TEXT("new")),
	syntax_while(allocator, OS_TEXT("while")),
	syntax_do(allocator, OS_TEXT("do")),
	syntax_const(allocator, OS_TEXT("const")),
	syntax_public(allocator, OS_TEXT("public")),
	syntax_protected(allocator, OS_TEXT("protected")),
	syntax_private(allocator, OS_TEXT("private")),
	syntax_yield(allocator, OS_TEXT("yield")),
	syntax_static(allocator, OS_TEXT("static")),
	syntax_debugger(allocator, OS_TEXT("debugger")),
	syntax_debuglocals(allocator, OS_TEXT("debuglocals")),
#ifdef OS_GLOBAL_VAR_ENABLED
	var_globals(allocator, OS_GLOBALS_VAR_NAME),
#endif
	var_env(allocator, OS_ENV_VAR_NAME),
	var_temp_prefix(allocator, OS_TEXT("#")),

	__dummy__(0)
{
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::MemoryManager::MemoryManager()
{
	ref_count = 1;
}

OS::MemoryManager::~MemoryManager()
{
}

OS::MemoryManager * OS::MemoryManager::retain()
{
	ref_count++;
	return this;
}

void OS::MemoryManager::release()
{
	if(--ref_count <= 0){
		OS_ASSERT(ref_count == 0);
		delete this;
	}
}

bool OS::isFileExist(const OS_CHAR * filename)
{
	void * f = openFile(filename, "rb");
	if(f){
		closeFile(f);
		return true;
	}
	return false;
}

void * OS::openFile(const OS_CHAR * filename, const OS_CHAR * mode)\
{
	return fopen(filename, mode);
}

int OS::readFile(void * buf, int size, void * f)
{
	if(f){
		return fread(buf, size, 1, (FILE*)f);
	}
	return 0;
}

int OS::writeFile(const void * buf, int size, void * f)
{
	if(f){
		return fwrite(buf, size, 1, (FILE*)f);
	}
	return 0;
}

int OS::seekFile(void * f, int offset, int whence)
{
	if(f){
		fseek((FILE*)f, offset, whence);
		return ftell((FILE*)f);
	}
	return 0;
}

void OS::closeFile(void * f)
{
	if(f){
		fclose((FILE*)f);
	}
}

void OS::printf(const OS_CHAR * format, ...)
{
	va_list va;
	va_start(va, format);
	OS_VPRINTF(format, va);
	va_end(va);
}

OS::SmartMemoryManager::SmartMemoryManager()
{
	allocated_bytes = 0;
	max_allocated_bytes = 0;
	cached_bytes = 0;
	OS_MEMSET(page_desc, 0, sizeof(page_desc));
	num_page_desc = 0;
	OS_MEMSET(pages, 0, sizeof(pages));
	OS_MEMSET(cached_blocks, 0, sizeof(cached_blocks));

#ifdef OS_DEBUG
	dbg_mem_list = NULL;
	dbg_std_mem_list = NULL;
	dbg_breakpoint_id = -1;
#endif

	stat_malloc_count = 0;
	stat_free_count = 0;

	registerPageDesc(sizeof(Core::GCObjectValue), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::GCStringValue), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::GCUserdataValue), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::GCFunctionValue), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::GCCFunctionValue), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::GCCFunctionValue) + sizeof(Core::Value)*4, OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::Property), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	// registerPageDesc(sizeof(Core::StackFunction), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::Upvalues), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::Upvalues) + sizeof(void*)*4, OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::Upvalues) + sizeof(void*)*8, OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::Table), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::Compiler::EXPRESSION_SIZE), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(sizeof(Core::TokenData), OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(8, OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(16, OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(32, OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(64, OS_MEMORY_MANAGER_PAGE_BLOCKS);
	registerPageDesc(128, OS_MEMORY_MANAGER_PAGE_BLOCKS/2);
	registerPageDesc(256, OS_MEMORY_MANAGER_PAGE_BLOCKS/4);
	sortPageDesc();
}

OS::SmartMemoryManager::~SmartMemoryManager()
{
	freeCachedMemory(0);
#ifdef OS_DEBUG
	{
		for(MemBlock * mem = dbg_mem_list; mem; mem = mem->dbg_mem_next){
			OS_PRINTF("[LEAK] %d bytes, id: %d, line %d, %s\n", mem->block_size, mem->dbg_id, mem->dbg_line, mem->dbg_filename);
		}
	}
	{
		for(StdMemBlock * mem = dbg_std_mem_list; mem; mem = mem->dbg_mem_next){
			OS_ASSERT(mem->block_size & 0x80000000);
			OS_PRINTF("[LEAK] %d bytes, id: %d, line %d, %s\n", (mem->block_size & ~0x80000000), mem->dbg_id, mem->dbg_line, mem->dbg_filename);
		}
	}
#endif
	// OS_ASSERT(!allocated_bytes && !cached_bytes);
}

#ifdef OS_DEBUG
static const int MEM_MARK_BEGIN = 0xabcdef98;
static const int MEM_MARK_END = 0x3579faec;
static const int FREE_MARK_BEGIN = 0xdabcef98;
static const int FREE_MARK_END = 0x3faec579;
static const int STD_MEM_MARK_BEGIN = 0xaefbcd98;
static const int STD_MEM_MARK_END = 0x35ae79fc;
#define MEM_MARK_END_SIZE sizeof(int)
#else
#define MEM_MARK_END_SIZE 0
#endif

int OS::SmartMemoryManager::comparePageDesc(const void * pa, const void * pb)
{
	PageDesc * a = (PageDesc*)pa;
	PageDesc * b = (PageDesc*)pb;
	return a->block_size - b->block_size;
}

void OS::SmartMemoryManager::sortPageDesc()
{
	::qsort(page_desc, num_page_desc, sizeof(page_desc[0]), comparePageDesc);
}

void OS::SmartMemoryManager::registerPageDesc(int block_size, int num_blocks)
{
	if(num_page_desc == MAX_PAGE_TYPE_COUNT){
		return;
	}
	if(block_size > 128){
		block_size = (block_size + 31) & ~31;
	}else if(block_size > 64){
		block_size = (block_size + 15) & ~15;
	}else if(block_size > 32){
		block_size = (block_size + 7) & ~7;
	}else{
		block_size = (block_size + 3) & ~3;
	}
	int i;
	for(i = 0; i < num_page_desc; i++){
		if(page_desc[i].block_size == block_size){
			if(page_desc[i].num_blocks < num_blocks){
				page_desc[i].num_blocks = num_blocks;
				page_desc[i].allocated_bytes = sizeof(Page) + (sizeof(MemBlock) + block_size + MEM_MARK_END_SIZE) * num_blocks;
			}
			return;
		}
	}
	page_desc[i].block_size = block_size;
	page_desc[i].num_blocks = num_blocks;
	page_desc[i].allocated_bytes = sizeof(Page) + (sizeof(MemBlock) + block_size + MEM_MARK_END_SIZE) * num_blocks;
	num_page_desc++;
}

void * OS::SmartMemoryManager::allocFromCachedBlock(int i OS_DBG_FILEPOS_DECL)
{
#ifdef OS_DEBUG
	if(stat_malloc_count == dbg_breakpoint_id){
		DEBUG_BREAK;
	}
#endif
	stat_malloc_count++;
	OS_ASSERT(i >= 0 && i < num_page_desc);
	CachedBlock * cached_block = cached_blocks[i];
	OS_ASSERT(cached_block);
#ifdef OS_DEBUG
	OS_ASSERT(cached_block->mark == FREE_MARK_BEGIN);
	OS_ASSERT(*(int*)(((OS_BYTE*)((MemBlock*)cached_block+1)) + page_desc[i].block_size) == FREE_MARK_END);
#endif
	cached_blocks[i] = cached_block->next;
	Page * page = cached_block->page;
	OS_ASSERT(page->num_cached_blocks > 0);
	page->num_cached_blocks--;
	MemBlock * mem_block = (MemBlock*)cached_block;
	mem_block->page = page;
	mem_block->block_size = page_desc[i].block_size;
#ifdef OS_DEBUG
	mem_block->mark = MEM_MARK_BEGIN;
	*(int*)(((OS_BYTE*)(mem_block+1)) + mem_block->block_size) = MEM_MARK_END;

	mem_block->dbg_filename = dbg_filename;
	mem_block->dbg_line = dbg_line;
	mem_block->dbg_id = stat_malloc_count-1;

	mem_block->dbg_mem_prev = NULL;
	mem_block->dbg_mem_next = dbg_mem_list;
	if(dbg_mem_list){
		dbg_mem_list->dbg_mem_prev = mem_block;
	}
	dbg_mem_list = mem_block;
#endif
	cached_bytes -= mem_block->block_size + sizeof(MemBlock);
	void * p = mem_block + 1;
	OS_MEMSET(p, 0, mem_block->block_size);
	// OS_ASSERT(mem_block->mark == MEM_MARK_BEGIN);
	// OS_ASSERT(*(int*)(((OS_BYTE*)(mem_block+1)) + mem_block->block_size) == MEM_MARK_END);
	return p;
}

void * OS::SmartMemoryManager::allocFromPageType(int i OS_DBG_FILEPOS_DECL)
{
	OS_ASSERT(i >= 0 && i < num_page_desc);
	if(cached_blocks[i]){
		return allocFromCachedBlock(i OS_DBG_FILEPOS_PARAM);
	}

	int allocated_bytes = page_desc[i].allocated_bytes;
	Page * page = (Page*)stdAlloc(allocated_bytes OS_DBG_FILEPOS);
	page->index = i;
	page->next_page = pages[i];
	pages[i] = page;
	page->num_cached_blocks = page_desc[i].num_blocks;
	cached_bytes += allocated_bytes;

	OS_BYTE * next_page_block = (OS_BYTE*)(page + 1);
	for(int j = 0; j < page_desc[i].num_blocks; j++){
		CachedBlock * cached_block = (CachedBlock*)next_page_block;
		cached_block->page = page;
		cached_block->next = cached_blocks[i];
#ifdef OS_DEBUG
		cached_block->mark = FREE_MARK_BEGIN;
		*(int*)(((OS_BYTE*)((MemBlock*)cached_block+1)) + page_desc[page->index].block_size) = FREE_MARK_END;
		OS_MEMSET(cached_block+1, 0xde, page_desc[i].block_size + (sizeof(MemBlock) - sizeof(CachedBlock)));
#endif
		cached_blocks[i] = cached_block;
		next_page_block += sizeof(MemBlock) + page_desc[i].block_size + MEM_MARK_END_SIZE;
	}

	return allocFromCachedBlock(i OS_DBG_FILEPOS_PARAM);
}

void OS::SmartMemoryManager::freeMemBlock(MemBlock * mem_block)
{
	stat_free_count++;
#ifdef OS_DEBUG
	OS_ASSERT(mem_block->mark == MEM_MARK_BEGIN);
	OS_ASSERT(*(int*)(((OS_BYTE*)(mem_block+1)) + mem_block->block_size) == MEM_MARK_END);
	if(mem_block->dbg_id == dbg_breakpoint_id){
		DEBUG_BREAK;
	}
	if(mem_block == dbg_mem_list){
		OS_ASSERT(!mem_block->dbg_mem_prev);
		dbg_mem_list = mem_block->dbg_mem_next;
	}else{ // if(mem_block->dbg_mem_prev){
		OS_ASSERT(mem_block->dbg_mem_prev);
		mem_block->dbg_mem_prev->dbg_mem_next = mem_block->dbg_mem_next;
	}
	if(mem_block->dbg_mem_next){
		mem_block->dbg_mem_next->dbg_mem_prev = mem_block->dbg_mem_prev;
	}
#endif
	Page * page = mem_block->page;
	int size = mem_block->block_size;
	cached_bytes += size + sizeof(MemBlock);
	CachedBlock * cached_block = (CachedBlock*)mem_block;
	cached_block->page = page;
	cached_block->next = cached_blocks[page->index];
#ifdef OS_DEBUG
	cached_block->mark = FREE_MARK_BEGIN;
	*(int*)(((OS_BYTE*)((MemBlock*)cached_block+1)) + page_desc[page->index].block_size) = FREE_MARK_END;
	OS_MEMSET(cached_block+1, 0xde, size + (sizeof(MemBlock) - sizeof(CachedBlock)));
#endif
	cached_blocks[page->index] = cached_block;
	page->num_cached_blocks++;
}

void OS::SmartMemoryManager::freeCachedMemory(int new_cached_bytes)
{
	if(cached_bytes > new_cached_bytes){
		for(int i = num_page_desc-1; i >= 0; i--){
			bool found_free_page = false;
			int num_blocks = page_desc[i].num_blocks;
			CachedBlock * prev_cached_block = NULL, * next_cached_block = NULL;
			for(CachedBlock * cached_block = cached_blocks[i]; cached_block; cached_block = next_cached_block){
				OS_ASSERT(cached_block->page->index == i);
				next_cached_block = cached_block->next;
				if(cached_block->page->num_cached_blocks == num_blocks){
					found_free_page = true;
					if(!prev_cached_block){
						cached_blocks[i] = next_cached_block;
					}else{
						prev_cached_block->next = next_cached_block;
					}
					// keep prev_cached_block
					continue;
				}
				prev_cached_block = cached_block;
			}
			if(found_free_page){
				Page * prev = NULL, * next;
				for(Page * page = pages[i]; page; page = next){
					next = page->next_page;
					if(page->num_cached_blocks == num_blocks){
						if(!prev){
							pages[i] = page->next_page;
						}else{
							prev->next_page = page->next_page;
						}
						cached_bytes -= page_desc[i].allocated_bytes;
						stdFree(page);
						// stat_free_count++;
					}else{
						prev = page;
					}
				}
				if(cached_bytes <= new_cached_bytes){
					break;
				}
			}
		}
	}
}

void * OS::SmartMemoryManager::stdAlloc(int size OS_DBG_FILEPOS_DECL)
{
#ifdef OS_DEBUG
	if(stat_malloc_count == dbg_breakpoint_id){
		DEBUG_BREAK;
	}
#endif
	stat_malloc_count++;
	size = (size + 7) & ~7;
	StdMemBlock * mem_block = (StdMemBlock*)::malloc(size + sizeof(StdMemBlock) + MEM_MARK_END_SIZE);
	if(!mem_block && cached_bytes > 0){
		freeCachedMemory(0);
		mem_block = (StdMemBlock*)::malloc(size + sizeof(StdMemBlock) + MEM_MARK_END_SIZE);
		if(!mem_block){
			return NULL;
		}
	}
#ifdef OS_DEBUG
	mem_block->mark = STD_MEM_MARK_BEGIN;
	*(int*)(((OS_BYTE*)(mem_block+1)) + size) = STD_MEM_MARK_END;

	mem_block->dbg_filename = dbg_filename;
	mem_block->dbg_line = dbg_line;
	mem_block->dbg_id = stat_malloc_count-1;

	mem_block->dbg_mem_prev = NULL;
	mem_block->dbg_mem_next = dbg_std_mem_list;
	if(dbg_std_mem_list){
		dbg_std_mem_list->dbg_mem_prev = mem_block;
	}
	dbg_std_mem_list = mem_block;
#endif
	mem_block->block_size = size | 0x80000000;
	allocated_bytes += size + sizeof(StdMemBlock) + MEM_MARK_END_SIZE;
	if(max_allocated_bytes < allocated_bytes){
		max_allocated_bytes = allocated_bytes;
	}
	OS_MEMSET(mem_block+1, 0, size);
	return mem_block+1;
}

void OS::SmartMemoryManager::stdFree(void * ptr)
{
	stat_free_count++;
	StdMemBlock * mem_block = (StdMemBlock*)ptr - 1;
	OS_ASSERT(mem_block->block_size & 0x80000000);
#ifdef OS_DEBUG
	OS_ASSERT(mem_block->mark == STD_MEM_MARK_BEGIN);
	OS_ASSERT(*(int*)(((OS_BYTE*)(mem_block+1)) + (mem_block->block_size & ~0x80000000)) == STD_MEM_MARK_END);

	if(mem_block->dbg_id == dbg_breakpoint_id){
		DEBUG_BREAK;
	}
	if(mem_block == dbg_std_mem_list){
		OS_ASSERT(!mem_block->dbg_mem_prev);
		dbg_std_mem_list = mem_block->dbg_mem_next;
	}else{ // if(mem_block->dbg_mem_prev){
		OS_ASSERT(mem_block->dbg_mem_prev);
		mem_block->dbg_mem_prev->dbg_mem_next = mem_block->dbg_mem_next;
	}
	if(mem_block->dbg_mem_next){
		mem_block->dbg_mem_next->dbg_mem_prev = mem_block->dbg_mem_prev;
	}
#endif
	int size = mem_block->block_size & ~0x80000000;
	allocated_bytes -= size + sizeof(StdMemBlock) + MEM_MARK_END_SIZE;
#ifdef OS_DEBUG
	OS_MEMSET(ptr, 0xde, size);
#endif
	::free(mem_block);
}

void * OS::SmartMemoryManager::malloc(int size OS_DBG_FILEPOS_DECL)
{
	if(size <= 0){
		return NULL;
	}
	// stat_malloc_count++;
#if 0
	int start = 0, end = num_page_desc-1;
	if(size <= page_desc[end].block_size){
		for(;;){
			if(start >= end){
				int block_size = page_desc[start].block_size;
				if(size > block_size){
					start++;
				}
				return allocFromPageType(start);
			}
			int mid = (start + end) / 2;
			int block_size = page_desc[mid].block_size;
			if(size == block_size){
				return allocFromPageType(mid);
			}
			if(size < block_size){
				end = mid - 1;
				continue;
			}
			start = mid + 1;
		}
	}
#else
	if(size <= page_desc[num_page_desc-1].block_size){
		for(int i = 0; i < num_page_desc; i++){
			if(size <= page_desc[i].block_size){
				return allocFromPageType(i OS_DBG_FILEPOS_PARAM);
			}
		}
	}
#endif
	return stdAlloc(size OS_DBG_FILEPOS_PARAM);
}

void OS::SmartMemoryManager::free(void * ptr)
{
	if(!ptr){
		return;
	}
	// stat_free_count++;
#ifdef OS_DEBUG
	int * p = (int*)ptr - 2;
#else
	int * p = (int*)ptr - 1;
#endif
	int size = p[0];
	if(size & 0x80000000){
		stdFree(ptr); // p, size & ~0x80000000);
		return;
	}
	MemBlock * mem_block = (MemBlock*)ptr - 1;
	OS_ASSERT(mem_block->block_size == size);
	freeMemBlock(mem_block);
	if(!(stat_free_count % 1024) && cached_bytes > allocated_bytes / 2){
		freeCachedMemory(cached_bytes / 2);
	}
}

void OS::SmartMemoryManager::setBreakpointId(int id)
{
#ifdef OS_DEBUG
	dbg_breakpoint_id = id;
#endif
}

int OS::SmartMemoryManager::getAllocatedBytes()
{
	return allocated_bytes;
}

int OS::SmartMemoryManager::getMaxAllocatedBytes()
{
	return max_allocated_bytes;
}

int OS::SmartMemoryManager::getCachedBytes()
{
	return cached_bytes;
}

// =====================================================================
// =====================================================================
// =====================================================================

OS::OS()
{
	ref_count = 1;
	memory_manager = NULL;
	core = NULL;
#ifdef OS_DEBUG
	int mark = 0;
	native_stack_start_mark = (int)&mark;
	native_stack_max_usage = 0;
#endif
}

OS::~OS()
{
	OS_ASSERT(ref_count == 0);
	OS_ASSERT(!core && !memory_manager);
	// deleteObj(core);
	// delete memory_manager;
	// memory_manager->release();
}

#ifdef OS_DEBUG
void OS::checkNativeStackUsage(const OS_CHAR * func_name)
{
	int mark = 0;
	int cur_native_stack_usage = (int)&mark - native_stack_start_mark;
	if(cur_native_stack_usage < 0){
		// native_stack_start_mark += cur_native_stack_usage;
		cur_native_stack_usage = -cur_native_stack_usage;
	}
	if(native_stack_max_usage < cur_native_stack_usage){
		if(cur_native_stack_usage > 1024*10 && cur_native_stack_usage > native_stack_max_usage * 5 / 4){
			printf(OS_TEXT("native stack usage: %.1f Kb (%s)\n"), (float)cur_native_stack_usage/1024.0f, func_name);
		}
		native_stack_max_usage = cur_native_stack_usage;
	}
}
#endif

void * OS::malloc(int size OS_DBG_FILEPOS_DECL)
{
	return memory_manager->malloc(size OS_DBG_FILEPOS_PARAM);
}

void OS::free(void * p)
{
	memory_manager->free(p);
}

void * OS::Core::malloc(int size OS_DBG_FILEPOS_DECL)
{
	return allocator->malloc(size OS_DBG_FILEPOS_PARAM);
}

void OS::Core::free(void * p)
{
	allocator->free(p);
}

int OS::getAllocatedBytes()
{
	return memory_manager->getAllocatedBytes();
}

int OS::getMaxAllocatedBytes()
{
	return memory_manager->getMaxAllocatedBytes();
}

int OS::getCachedBytes()
{
	return memory_manager->getCachedBytes();
}

void OS::setMemBreakpointId(int id)
{
	memory_manager->setBreakpointId(id);
}

bool OS::isTerminated()
{
	return core->terminated;
}

int OS::getTerminatedCode()
{
	return core->terminated_code;
}

void OS::setTerminated(bool terminated, int code)
{
	core->terminated = terminated;
	core->terminated_code = code;
}

void OS::resetTerminated()
{
	core->terminated = false;
	core->terminated_code = 0;
}

OS::Core::Core(OS * p_allocator)
{
	allocator = p_allocator;
	strings = NULL;
	OS_MEMSET(prototypes, 0, sizeof(prototypes));

	// string_values_table = NULL;
	check_recursion = NULL;
	// global_vars = NULL;
	// user_pool = NULL;

	num_created_values = 0;
	num_destroyed_values = 0;

	stack_func = NULL;
	stack_func_locals = NULL;
	num_stack_func_locals = 0;
	stack_func_env_index = 0;
	stack_func_prog_numbers = NULL;
	stack_func_prog_strings = NULL;

	settings.create_compiled_file = true;
	settings.create_debug_info = true;
	settings.create_debug_opcodes = true;
	settings.primary_compiled_file = false;

	gcInitGreyList();

	OS_MEMSET(rand_state, 0, sizeof(rand_state));
	rand_next = NULL;
	rand_seed = 0;
	rand_left = 0;

	terminated = false;
	terminated_code = 0;
}

OS::Core::~Core()
{
	OS_ASSERT(!strings && global_vars.isNull() && user_pool.isNull() && !check_recursion);
	for(int i = 0; i < PROTOTYPE_COUNT; i++){
		OS_ASSERT(!prototypes[i]);
	}
}

OS * OS::create(MemoryManager * manager)
{
	return create(new OS(), manager);
}

/*
OS * OS::create(OS * os, MemoryManager * manager)
{
return os->start(manager);
}
*/

OS * OS::start(MemoryManager * manager)
{
	if(init(manager)){
		return this;
	}
	delete this;
	return NULL;
}

bool OS::init(MemoryManager * p_manager)
{
	memory_manager = p_manager ? p_manager : new SmartMemoryManager();
	core = new (malloc(sizeof(Core) OS_DBG_FILEPOS)) Core(this);

	if(core->init()){
#if 0
		initPreScript();
		initGlobalFunctions();
		initObjectClass();
		initArrayClass();
		initStringClass();
		initFunctionClass();
		initMathModule();
		initGCModule();
		initLangTokenizerModule();
		initPostScript();
#endif
		return true;
	}
	return false;
}

void OS::shutdown()
{
	core->shutdown();
	core->~Core();
	free(core);
	core = NULL;

	memory_manager->release();
	memory_manager = NULL;
}

OS * OS::retain()
{
	ref_count++;
	return this;
}

void OS::release()
{
	if(--ref_count <= 0){
		OS_ASSERT(ref_count == 0);
		shutdown();
		delete this;
	}
}

bool OS::Core::init()
{
	// string_values_table = newTable(OS_DBG_FILEPOS_START);
	int i;
	for(i = 0; i < PROTOTYPE_COUNT; i++){
		prototypes[i] = newObjectValue(NULL);
		prototypes[i]->type = OS_VALUE_TYPE_OBJECT;
		prototypes[i]->external_ref_count++;
	}
	check_recursion = newObjectValue();
	global_vars = newObjectValue();
	user_pool = newObjectValue();
	// error_handlers

	prototypes[PROTOTYPE_BOOL]->prototype = prototypes[PROTOTYPE_OBJECT];
	prototypes[PROTOTYPE_NUMBER]->prototype = prototypes[PROTOTYPE_OBJECT];
	prototypes[PROTOTYPE_STRING]->prototype = prototypes[PROTOTYPE_OBJECT];
	prototypes[PROTOTYPE_ARRAY]->prototype = prototypes[PROTOTYPE_OBJECT];
	prototypes[PROTOTYPE_FUNCTION]->prototype = prototypes[PROTOTYPE_OBJECT];
	prototypes[PROTOTYPE_USERDATA]->prototype = prototypes[PROTOTYPE_OBJECT];

	strings = new (malloc(sizeof(Strings) OS_DBG_FILEPOS)) Strings(allocator);

	setGlobalValue(OS_TEXT("Object"), Value(prototypes[PROTOTYPE_OBJECT]), false, false);
	setGlobalValue(OS_TEXT("Boolean"), Value(prototypes[PROTOTYPE_BOOL]), false, false);
	setGlobalValue(OS_TEXT("Number"), Value(prototypes[PROTOTYPE_NUMBER]), false, false);
	setGlobalValue(OS_TEXT("String"), Value(prototypes[PROTOTYPE_STRING]), false, false);
	setGlobalValue(OS_TEXT("Array"), Value(prototypes[PROTOTYPE_ARRAY]), false, false);
	setGlobalValue(OS_TEXT("Function"), Value(prototypes[PROTOTYPE_FUNCTION]), false, false);
	setGlobalValue(OS_TEXT("Userdata"), Value(prototypes[PROTOTYPE_USERDATA]), false, false);

	/*
		SAFE usage of user function arguments 
		so user can use just os->toNumber(-params+3) and so on
		if function call has no enough arguments, for example params == 0
		then (-params+3) will be not relative offset but absolute offset 3
		lets make top OS_TOP_STACK_NULL_VALUES value as null values
	*/
	for(i = 0; i < OS_TOP_STACK_NULL_VALUES; i++){
		pushValue(Value());
	}

	return true;
}

int OS::Core::compareGCValues(const void * a, const void * b)
{
	GCValue * v1 = *(GCValue**)a;
	GCValue * v2 = *(GCValue**)b;
	if(v1->external_ref_count != v2->external_ref_count){
		return v2->external_ref_count - v1->external_ref_count;
	}
	return v1->value_id - v2->value_id;
}

void OS::Core::shutdown()
{
	int i;
	OS_ASSERT(stack_values.count >= OS_TOP_STACK_NULL_VALUES);
	for(i = 0; i < OS_TOP_STACK_NULL_VALUES; i++){
		OS_ASSERT(stack_values[i].type == OS_VALUE_TYPE_NULL);
	}
	// stack_values.count = 0;
	while(call_stack_funcs.count > 0){
		StackFunction * stack_func = &call_stack_funcs[--call_stack_funcs.count];
		clearStackFunction(stack_func);
	}
	allocator->vectorClear(call_stack_funcs);
	// vectorClear(cache_values);

	// gcFull();
	gcResetGreyList();

	allocator->deleteObj(strings);

	// try to finalize the values accurately
	Vector<GCValue*> collectedValues;
	allocator->vectorReserveCapacity(collectedValues, values.count OS_DBG_FILEPOS);
	for(int i = 0; i <= values.head_mask; i++){
		for(GCValue * value = values.heads[i]; value; value = value->hash_next){
			allocator->vectorAddItem(collectedValues, value OS_DBG_FILEPOS);
		}
	}
	::qsort(collectedValues.buf, collectedValues.count, sizeof(GCValue*), compareGCValues);
	for(i = collectedValues.count-1; i >= 0; i--){
		deleteValue(collectedValues[i]);
	}
	allocator->vectorClear(collectedValues);
	deleteValues(true); // just clear values.heads

	check_recursion = NULL;
	global_vars = (GCValue*)NULL;
	user_pool = (GCValue*)NULL;

	for(i = 0; i < OS_ERROR_LEVELS; i++){
		error_handlers[i] = NULL;
	}
	for(i = 0; i < PROTOTYPE_COUNT; i++){
		prototypes[i] = NULL;
	}
	deleteStringRefs();
	deleteUserptrRefs();
	if(stack_values.buf){ // it makes sense because of someone could use stack while the finalizing in process
		free(stack_values.buf);
		stack_values.buf = NULL;
		stack_values.capacity = 0;
		stack_values.count = 0;
	}
	OS_ASSERT(!call_stack_funcs.count);
}

OS::String OS::changeFilenameExt(const String& filename, const String& ext)
{
	int len = filename.getLen();
	for(int i = len-1; i >= 0; i--){
		if(filename[i] == OS_TEXT('.')){
			return String(this, filename, i, ext, ext.getLen());
		}
		if(OS_IS_SLASH(filename[i])){
			break;
		}
	}
	return String(this, filename, len, ext, ext.getLen());
}

OS::String OS::changeFilenameExt(const String& filename, const OS_CHAR * ext)
{
	int len = filename.getLen();
	for(int i = len-1; i >= 0; i--){
		if(filename[i] == OS_TEXT('.')){
			if(OS_STRCMP(filename.toChar()+i, ext) == 0){
				return filename;
			}
			return String(this, filename, i, ext, OS_STRLEN(ext));
		}
		if(OS_IS_SLASH(filename[i])){
			break;
		}
	}
	return String(this, filename, len, ext, OS_STRLEN(ext));
}

OS::String OS::getFilenameExt(const String& filename)
{
	return getFilenameExt(filename, filename.getLen());
}

OS::String OS::getFilenameExt(const OS_CHAR * filename)
{
	return getFilenameExt(filename, OS_STRLEN(filename));
}

OS::String OS::getFilenameExt(const OS_CHAR * filename, int len)
{
	for(int i = len-1; i >= 0; i--){
		if(filename[i] == OS_TEXT('.')){
			return String(this, filename+i, len-i);
		}
		if(OS_IS_SLASH(filename[i])){
			break;
		}
	}
	return String(this);
}

OS::String OS::getFilename(const String& filename)
{
	return getFilename(filename, filename.getLen());
}

OS::String OS::getFilename(const OS_CHAR * filename)
{
	return getFilename(filename, OS_STRLEN(filename));
}

OS::String OS::getFilename(const OS_CHAR * filename, int len)
{
	for(int i = len-1; i >= 0; i--){
		if(OS_IS_SLASH(filename[i])){
			return String(this, filename+i+1, len-i-1);
		}
	}
	return String(this, filename, len);
}

OS::String OS::getFilenamePath(const String& filename)
{
	return getFilenamePath(filename, filename.getLen());
}

OS::String OS::getFilenamePath(const OS_CHAR * filename)
{
	return getFilenamePath(filename, OS_STRLEN(filename));
}

OS::String OS::getFilenamePath(const OS_CHAR * filename, int len)
{
	for(int i = len-1; i >= 0; i--){
		if(OS_IS_SLASH(filename[i])){
			return String(this, filename, i);
		}
	}
	return String(this);
}

bool OS::isAbsolutePath(const String& p_filename)
{
	int len = p_filename.getLen();
	const OS_CHAR * filename = p_filename;
	if(OS_IS_ALPHA(filename[0])){
		for(int i = 1; i < len-2; i++){
			if(!OS_IS_ALPHA(filename[i])){
				return filename[i] == OS_TEXT(':') && OS_IS_SLASH(filename[i+1]);
			}
		}
	}
	return len >= 2 && OS_IS_SLASH(filename[0]) && OS_IS_SLASH(filename[1]);
}

OS::String OS::resolvePath(const String& filename, const String& cur_path)
{
	String resolved_path = filename;
	if(!isAbsolutePath(filename) && cur_path.getLen()){
		if(filename.getLen() < cur_path.getLen() || String(this, filename.toChar(), cur_path.getLen()) != cur_path){
			resolved_path = cur_path + OS_PATH_SEPARATOR + filename;
		}
	}
	resolved_path = changeFilenameExt(resolved_path, OS_SOURCECODE_EXT);
	if(isFileExist(resolved_path)){
		return resolved_path;
	}
	resolved_path = changeFilenameExt(resolved_path, OS_COMPILED_EXT);
	if(isFileExist(resolved_path)){
		return resolved_path;
	}
	core->error(OS_E_WARNING, String::format(this, OS_TEXT("filename %s is not resolved"), filename.toChar()));
	return String(this);
}

OS::String OS::getCompiledFilename(const OS::String& resolved_filename)
{
	return changeFilenameExt(resolved_filename, OS_COMPILED_EXT);
}

OS::String OS::getDebugInfoFilename(const String& resolved_filename)
{
	return changeFilenameExt(resolved_filename, OS_DEBUG_INFO_EXT);
}

OS::String OS::getDebugOpcodesFilename(const String& resolved_filename)
{
	if(resolved_filename.getDataSize()){
		return changeFilenameExt(resolved_filename, OS_DEBUG_OPCODES_EXT);
	}
	static int num_evals = 0;
	return String(this, Core::String::format(this, OS_TEXT("eval-%d%s"), ++num_evals, OS_DEBUG_OPCODES_EXT));
}

OS::String OS::resolvePath(const String& filename)
{
	String cur_path(this);
	if(core->call_stack_funcs.count > 0){
		for(int i = core->call_stack_funcs.count-1; i >= 0; i--){
			Core::StackFunction * stack_func = core->call_stack_funcs.buf + i;
			if(stack_func->func->prog->filename.getLen() > 0){
				cur_path = getFilenamePath(stack_func->func->prog->filename);
				break;
			}
		}
	}
	return resolvePath(filename, cur_path);
}

OS::EFileUseType OS::checkFileUsage(const String& sourcecode_filename, const String& compiled_filename)
{
	return COMPILE_SOURCECODE_FILE;
}

void OS::Core::errorDivisionByZero()
{
	error(OS_E_WARNING, OS_TEXT("division by zero"));
}

void OS::Core::error(int code, const OS_CHAR * message)
{
	error(code, String(allocator, message));
}

void OS::Core::error(int code, const String& message)
{
	Program * prog = NULL;
	Program::DebugInfoItem * debug_info = NULL;
	for(int i = call_stack_funcs.count-1; i >= 0 && !debug_info; i--){
		Core::StackFunction * stack_func = call_stack_funcs.buf + i;
		prog = stack_func->func->prog;
		if(prog->filename.getLen() > 0){
			int opcode_pos = stack_func->opcodes.getPos() + stack_func->func->func_decl->opcodes_pos;
			debug_info = prog->getDebugInfo(opcode_pos);
		}
	}
	int error_level = 0;
	for(int i = 0; i < OS_ERROR_LEVELS; i++){
		if(code & (1<<i)){
			error_level = i;
			break;
		}
	}
	if(error_handlers[error_level].isFunction()){
		pushValue(error_handlers[error_level]);
		pushNull();
		pushNumber(code);
		pushStringValue(message);
		if(debug_info){
			pushStringValue(prog->filename);
			pushNumber(debug_info->line);
			call(4, 0);
		}else{
			call(2, 0);
		}
		return;
	}
	const OS_CHAR * error_type = NULL;
	switch(code){
	case OS_E_WARNING:
		error_type = OS_TEXT("WARNING");
		break;

	default:
	case OS_E_ERROR:
		error_type = OS_TEXT("ERROR");
		code = OS_E_ERROR;
		break;
	}
	if(debug_info){
		allocator->printf("[%s] %s (line: %d, pos: %d, token: %s, filename: %s)\n", error_type, message.toChar(), debug_info->line, debug_info->pos, 
			debug_info->token.toChar(), prog->filename.toChar());
	}else{
		allocator->printf("[%s] %s\n", error_type, message.toChar());
	}
}

void OS::Core::gcInitGreyList()
{
	gc_grey_list_first = NULL;
	gc_grey_root_initialized = false;
	gc_start_allocated_bytes = 0;
	gc_max_allocated_bytes = 0;
	gc_keep_heap_count = 0;
	gc_continuous_count = 0;
	gc_continuous = false;
	gc_values_head_index = -1;
	gc_time = 0;
	gc_in_process = false;
	gc_grey_added_count = 0;
	// gc_grey_removed_count = 0;
	gc_start_values_mult = 1.5f;
	gc_step_size_mult = 0.005f;
	gc_step_size_auto_mult = 1.0f;
	gc_start_next_values = 16;
	gc_step_size = 0;
}

void OS::Core::gcResetGreyList()
{
	while(gc_grey_list_first){
		gcRemoveFromGreyList(gc_grey_list_first);
	}
	gc_grey_root_initialized = false;
	// OS_ASSERT(gc_grey_list.gc_grey_next == (Value*)&gc_grey_list);
	// OS_ASSERT(gc_grey_list.gc_grey_prev == (Value*)&gc_grey_list);
}

void OS::Core::gcMarkList(int step_size)
{
	if(step_size < 16){
		step_size = 16;
	}
	for(; step_size > 0 && gc_grey_list_first; step_size--){
		gcMarkValue(gc_grey_list_first);
	}
}

void OS::Core::gcMarkTable(Table * table)
{
	Property * prop = table->first, * prop_next;
	for(; prop; prop = prop_next){
		prop_next = prop->next;
		if(prop->index.type == OS_VALUE_TYPE_WEAKREF){
			OS_ASSERT(false);
			if(!values.get(prop->index.v.value_id)){
				PropertyIndex index = *prop;
				deleteTableProperty(table, index);
				continue;
			}
		}
		if(prop->value.type == OS_VALUE_TYPE_WEAKREF){
			if(!values.get(prop->value.v.value_id)){
				PropertyIndex index = *prop;
				deleteTableProperty(table, index);
				continue;
			}
		}
		gcAddToGreyList(prop->index);
		gcAddToGreyList(prop->value);
	}
}

void OS::Core::gcMarkProgram(Program * prog)
{
	/* if(prog->gc_time == gc_time){
	return;
	}
	prog->gc_time = gc_time; */
	/* for(int i = 0; i < prog->num_strings; i++){
	gcAddToGreyList(prog->const_strings[i]);
	} */
}

void OS::Core::gcMarkUpvalues(Upvalues * upvalues)
{
	if(upvalues->gc_time == gc_time){
		return;
	}
	upvalues->gc_time = gc_time;

	int i;
	for(i = 0; i < upvalues->num_locals; i++){
		gcAddToGreyList(upvalues->locals[i]);
	}
	for(i = 0; i < upvalues->num_parents; i++){
		gcMarkUpvalues(upvalues->getParent(i));
	}
}

void OS::Core::gcMarkStackFunction(StackFunction * stack_func)
{
	OS_ASSERT(stack_func->func && stack_func->func->type == OS_VALUE_TYPE_FUNCTION);

	gcAddToGreyList(stack_func->func);
	gcAddToGreyList(stack_func->self);
	if(stack_func->self_for_proto){
		gcAddToGreyList(stack_func->self_for_proto);
	}

	gcMarkUpvalues(stack_func->locals);

	if(stack_func->arguments){
		gcAddToGreyList(stack_func->arguments);
	}
	if(stack_func->rest_arguments){
		gcAddToGreyList(stack_func->rest_arguments);
	}
}

void OS::Core::gcAddToGreyList(Value val)
{
	switch(val.type){
	case OS_VALUE_TYPE_STRING:
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		gcAddToGreyList(val.v.value);
		break;
	}
}

void OS::Core::gcAddToGreyList(GCValue * value)
{
	if(value->gc_color != GC_WHITE){
		return;
	}
	// OS_ASSERT(!value->gc_grey_next && !value->gc_grey_prev);
	OS_ASSERT(!value->gc_grey_next);
	// gc_grey_list.insertEnd(value);
	value->gc_grey_next = gc_grey_list_first;
	gc_grey_list_first = value;
	value->gc_color = GC_GREY;
	gc_grey_added_count++;
}

void OS::Core::gcRemoveFromGreyList(GCValue * value)
{
	// OS_ASSERT(value->gc_grey_next && value->gc_grey_prev);
	OS_ASSERT(value->gc_color == GC_GREY);
	OS_ASSERT(gc_grey_list_first == value);
	// gc_grey_list.remove(value);
	gc_grey_list_first = value->gc_grey_next;
	value->gc_grey_next = NULL;
	value->gc_color = GC_BLACK;
	// gc_grey_removed_count++;
}

void OS::Core::gcMarkValue(GCValue * value)
{
	gcRemoveFromGreyList(value);
	if(value->prototype){
		gcAddToGreyList(value->prototype);
	}
	if(value->table){
		gcMarkTable(value->table);
	}
	switch(value->type){
	case OS_VALUE_TYPE_NULL:
	case OS_VALUE_TYPE_BOOL:
	case OS_VALUE_TYPE_NUMBER:
	default:
		OS_ASSERT(false);
		break;

	case OS_VALUE_TYPE_STRING:
		OS_ASSERT(dynamic_cast<GCStringValue*>(value));
		break;

	case OS_VALUE_TYPE_OBJECT:
		OS_ASSERT(dynamic_cast<GCObjectValue*>(value));
		break;

	case OS_VALUE_TYPE_ARRAY:
		{
			OS_ASSERT(dynamic_cast<GCArrayValue*>(value));
			GCArrayValue * arr = (GCArrayValue*)value;
			for(int i = 0; i < arr->values.count; i++){
				gcAddToGreyList(arr->values[i]);
			}
			break;
		}

	case OS_VALUE_TYPE_FUNCTION:
		{
			OS_ASSERT(dynamic_cast<GCFunctionValue*>(value));
			GCFunctionValue * func_value = (GCFunctionValue*)value;
			gcMarkProgram(func_value->prog);
			gcAddToGreyList(func_value->env);
			if(func_value->upvalues){
				gcMarkUpvalues(func_value->upvalues);
			}
			if(func_value->name){
				gcAddToGreyList(func_value->name);
			}
			break;
		}

	case OS_VALUE_TYPE_CFUNCTION:
		{
			OS_ASSERT(dynamic_cast<GCCFunctionValue*>(value));
			GCCFunctionValue * func_value = (GCCFunctionValue*)value;
			Value * closure_values = (Value*)(func_value + 1);
			for(int i = 0; i < func_value->num_closure_values; i++){
				gcAddToGreyList(closure_values[i]);
			}
			if(func_value->name){
				gcAddToGreyList(func_value->name);
			}
			break;
		}

	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		OS_ASSERT(dynamic_cast<GCUserdataValue*>(value));
		break;
	}
}

void OS::onEnterGC()
{
}

void OS::onExitGC()
{
}

int OS::Core::gcStep()
{
	// return OS_GC_PHASE_MARK;
	if(gc_in_process){
		return OS_GC_PHASE_MARK;
	}
	struct GCTouch {
		Core * core;
		GCTouch(Core * p_core)
		{
			core = p_core; core->gc_in_process = true;
			core->allocator->onEnterGC();
		}
		~GCTouch()
		{
			core->gc_in_process = false;
			core->allocator->onExitGC();
		}
	} gc_touch(this);

	if(values.count == 0){
		gc_values_head_index = -1;
		gc_grey_root_initialized = false;
		gc_continuous = false;
		return OS_GC_PHASE_MARK;
	}
	int step_size = gc_step_size;
	if(gc_values_head_index >= 0){
		OS_ASSERT(gc_values_head_index <= values.head_mask);
		int i = gc_values_head_index;
		step_size += 2; // step_size/16;
		for(; i <= values.head_mask && step_size > 0; i++){
			for(GCValue * value = values.heads[i], * next; value; value = next, step_size--){
				next = value->hash_next;
				if(value->gc_color == GC_WHITE && !value->external_ref_count){
					OS_ASSERT(!isValueUsed(value));
					deleteValue(value);
					if(gc_values_head_index < 0){
						return OS_GC_PHASE_MARK;
					}
				}else if(value->gc_color == GC_BLACK){
					value->gc_color = GC_WHITE;
				}
			}
		}
		if(i <= values.head_mask){
			gc_values_head_index = i;
			gc_step_size_auto_mult *= 1.01f;
			gc_step_size = (int)((float)values.count * gc_step_size_mult * gc_step_size_auto_mult * 2);
			return OS_GC_PHASE_SWEEP;
		}
		gc_values_head_index = -1;
		gc_start_next_values = (int)((float)values.count * gc_start_values_mult);

		int end_allocated_bytes = allocator->getAllocatedBytes();
		gc_continuous_count++;
		if(gc_start_allocated_bytes == end_allocated_bytes){
			gc_step_size_auto_mult *= 0.5f;
			if(gc_step_size_auto_mult < 1){
				gc_step_size_auto_mult = 1.0f;
			}
			if(++gc_keep_heap_count >= 2){
				gc_continuous = false;
				// gc_step_size_auto_mult = 1.0f;
			}
		}else{
			gc_start_allocated_bytes = end_allocated_bytes;
			gc_keep_heap_count = 0;
		}

		if((!gc_continuous || !(gc_continuous_count%16)) && gc_max_allocated_bytes < end_allocated_bytes){
			gc_max_allocated_bytes = end_allocated_bytes;
			// allocator->printf("[GC] max allocated bytes %d, values %d\n", gc_max_allocated_bytes, values.count);
		}

		return OS_GC_PHASE_MARK;
	}
	if(!gc_grey_root_initialized){
		gc_grey_root_initialized = true;
		gc_step_size = (int)((float)values.count * gc_step_size_mult * gc_step_size_auto_mult * 2);
		gc_time++;

		if(!gc_continuous){
			gc_continuous = true;
			gc_continuous_count = 0;
			gc_keep_heap_count = 0;
			// gc_start_allocated_bytes = allocator->getAllocatedBytes();
			gc_step_size_auto_mult = 1.0f;
		}else{
			// int i = 0;
		}

		// int old_count = gc_grey_added_count;
		gcAddToGreyList(check_recursion);
		gcAddToGreyList(global_vars);
		gcAddToGreyList(user_pool);
		int i;
		for(i = 0; i < OS_ERROR_LEVELS; i++){
			gcAddToGreyList(error_handlers[i]);
		}
		for(i = 0; i < PROTOTYPE_COUNT; i++){
			gcAddToGreyList(prototypes[i]);
		}
		// gcMarkTable(string_values_table);
		// step_size -= gc_grey_added_count - old_count;
	}
	int i;
	for(i = 0; i < stack_values.count; i++){
		gcAddToGreyList(stack_values[i]);
	}
	for(i = 0; i < call_stack_funcs.count; i++){
		gcMarkStackFunction(&call_stack_funcs[i]);
	}
	gcMarkList(step_size);
	gc_step_size = (int)((float)values.count * gc_step_size_mult * gc_step_size_auto_mult * 2);
	if(!gc_grey_list_first){
		gc_grey_root_initialized = false;
		gc_values_head_index = 0;
		gc_step_size_auto_mult *= 0.25f;
		if(gc_step_size_auto_mult < 1.0f){
			gc_step_size_auto_mult = 1.0f;
		}
		return OS_GC_PHASE_SWEEP;
	}
	gc_step_size_auto_mult *= 1.01f;
	return OS_GC_PHASE_MARK;
}

void OS::Core::gcFinishSweepPhase()
{
	if(gc_in_process || values.count == 0){
		return;
	}
	if(gc_values_head_index >= 0){
		gc_step_size = values.count * 2;
		gcStep();
		OS_ASSERT(gc_values_head_index < 0);
	}
}

void OS::Core::gcFinishMarkPhase()
{
	if(gc_in_process || values.count == 0){
		return;
	}
	while(gc_values_head_index < 0){
		gc_step_size = values.count * 2;
		gcStep();
	}
}

void OS::Core::gcStepIfNeeded()
{
	if(gc_in_process){
		return;
	}
	if(gc_values_head_index >= 0 || gc_grey_root_initialized || gc_continuous){
		gcStep();
	}else if(gc_start_next_values <= values.count){
		gcFinishSweepPhase();
		gcStep();
	}
}

void OS::Core::gcFull()
{
	if(gc_in_process){
		return;
	}
	gcFinishSweepPhase();
	int start_allocated_bytes = allocator->getAllocatedBytes();
	for(int i = 1;; i++){
		gcFinishMarkPhase();
		gcFinishSweepPhase();
		int end_allocated_bytes = allocator->getAllocatedBytes();
		if(start_allocated_bytes == end_allocated_bytes && i > 1){
			return;
		}
		start_allocated_bytes = end_allocated_bytes;
	}
}

/*
void OS::Core::clearValue(Value& val)
{
switch(val.type){
case OS_VALUE_TYPE_NULL:
return;

case OS_VALUE_TYPE_BOOL:
val.v.boolean = 0;
break;

case OS_VALUE_TYPE_NUMBER:
val.v.number = 0;
break;

case OS_VALUE_TYPE_WEAKREF:
val.v.value_id = 0;
break;

default:
val.v.value = 0;
}
val.type = OS_VALUE_TYPE_NULL;
}
*/

void OS::Core::clearValue(GCValue * val)
{
	switch(val->type){
	case OS_VALUE_TYPE_NULL:
	case OS_VALUE_TYPE_BOOL:
	case OS_VALUE_TYPE_NUMBER:
	default:
		OS_ASSERT(false);
		break;

	case OS_VALUE_TYPE_STRING:
		{
			OS_ASSERT(dynamic_cast<GCStringValue*>(val));
			break;
		}

	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		{
			OS_ASSERT(dynamic_cast<GCUserdataValue*>(val));
			GCUserdataValue * userdata = (GCUserdataValue*)val;

			void * ptr = userdata->ptr;
			OS_UserdataDtor dtor  = userdata->dtor;

			// prevent recursion
			userdata->ptr = NULL;
			userdata->crc = 0;
			userdata->dtor = NULL;

			if(val->type == OS_VALUE_TYPE_USERPTR){
				unregisterUserptrRef(ptr, userdata->value_id);
			}
			if(dtor){
				dtor(allocator, ptr, userdata->user_param);
			}
			break;
		}

	case OS_VALUE_TYPE_FUNCTION:
		{
			OS_ASSERT(dynamic_cast<GCFunctionValue*>(val));
			GCFunctionValue * func_value = (GCFunctionValue*)val;
			clearFunctionValue(func_value);
			break;
		}

	case OS_VALUE_TYPE_CFUNCTION:
		{
			OS_ASSERT(dynamic_cast<GCCFunctionValue*>(val));
			GCCFunctionValue * func_value = (GCCFunctionValue*)val;
			func_value->func = NULL;
			func_value->user_param = NULL;
			func_value->num_closure_values = 0;
			break;
		}

	case OS_VALUE_TYPE_ARRAY:
		{
			OS_ASSERT(dynamic_cast<GCArrayValue*>(val));
			GCArrayValue * arr = (GCArrayValue*)val;
			allocator->vectorClear(arr->values);
			break;
		}

	case OS_VALUE_TYPE_OBJECT:
		OS_ASSERT(dynamic_cast<GCObjectValue*>(val));
		break;
	}
	if(val->table){
		// when object is destroying, some properties could be already destroyed
		// so destructor can't use self properties and can break gc process
		// so destructors are disabled
		/* {
		bool prototype_enabled = true;
		Value * func = getPropertyValue(val, PropertyIndex(strings->__destruct, PropertyIndex::KeepStringIndex()), prototype_enabled);
		if(func){
		pushValue(func);
		pushValue(val);
		call(0, 0);
		}
		} */

		Table * table = val->table;
		val->table = NULL;
		deleteTable(table);
	}
	if(val->prototype){
		// prototype could be already destroyed by gc or will be destroyed soon
		val->prototype = NULL;
	}
	val->type = OS_VALUE_TYPE_UNKNOWN;
}

#ifdef OS_DEBUG
bool OS::Core::isValueUsed(GCValue * val)
{
	struct Lib {
		Core * core;
		GCValue * val;

		bool findAt(Value cur)
		{
			GCValue * value = cur.getGCValue();
			return value && findAt(value);
		}

		bool findAt(Upvalues * upvalues)
		{
			int i;
			for(i = 0; i < upvalues->num_locals; i++){
				if(findAt(upvalues->locals[i])){
					return true;
				}
			}
			for(i = 0; i < upvalues->num_parents; i++){
				if(findAt(upvalues->getParent(i))){
					return true;
				}
			}
			return false;
		}

		bool findAt(StackFunction * stack_func)
		{
			OS_ASSERT(stack_func->func);
			if(findAt(stack_func->func)){
				return true;
			}
			if(findAt(stack_func->self)){
				return true;
			}
			if(stack_func->self_for_proto && findAt(stack_func->self_for_proto)){
				return true;
			}
			if(stack_func->arguments && findAt(stack_func->arguments)){
				return true;
			}
			if(stack_func->rest_arguments && findAt(stack_func->rest_arguments)){
				return true;
			}
			return findAt(stack_func->locals);
		}

		bool findAt(Table * table)
		{
			OS_ASSERT(table);
			Property * prop = table->first;
			for(; prop; prop = prop->next){
				if(findAt(prop->index)){
					return true;
				}
				if(findAt(prop->value)){
					return true;
				}
			}
			return false;
		}

		bool findAt(GCValue * cur)
		{
			OS_ASSERT(cur != (GCValue*)0xdededede);
			if(cur->gc_time == core->gc_time){
				return false;
			}
			cur->gc_time = core->gc_time;

			if(cur == val){
				return true;
			}
			if(cur->prototype && findAt(cur->prototype)){
				return true;
			}
			if(cur->table && findAt(cur->table)){
				return true;
			}
			switch(cur->type){
			case OS_VALUE_TYPE_STRING:
				{
					OS_ASSERT(dynamic_cast<GCStringValue*>(cur));
					GCStringValue * string = (GCStringValue*)cur;
					OS_ASSERT(!string->table);
					break;
				}

			case OS_VALUE_TYPE_ARRAY:
				{
					OS_ASSERT(dynamic_cast<GCArrayValue*>(cur));
					GCArrayValue * arr = (GCArrayValue*)cur;
					for(int i = 0; i < arr->values.count; i++){
						if(findAt(arr->values[i])){
							return true;
						}
					}
					break;
				}

			case OS_VALUE_TYPE_OBJECT:
				OS_ASSERT(dynamic_cast<GCObjectValue*>(cur));
				break;

			case OS_VALUE_TYPE_USERDATA:
			case OS_VALUE_TYPE_USERPTR:
				OS_ASSERT(dynamic_cast<GCUserdataValue*>(cur));
				break;

			case OS_VALUE_TYPE_FUNCTION:
				{
					OS_ASSERT(dynamic_cast<GCFunctionValue*>(cur));
					GCFunctionValue * func_value = (GCFunctionValue*)cur;
					if(findAt(func_value->env)){
						return true;
					}
					if(func_value->upvalues && findAt(func_value->upvalues)){
						return true;
					}
					if(func_value->name && findAt(func_value->name)){
						return true;
					}
					for(int i = 0; i < func_value->prog->num_strings; i++){
						if(findAt(func_value->prog->const_strings[i])){
							return true;
						}
					}
					break;
				}

			case OS_VALUE_TYPE_CFUNCTION:
				{
					OS_ASSERT(dynamic_cast<GCCFunctionValue*>(cur));
					GCCFunctionValue * func_value = (GCCFunctionValue*)cur;
					Value * closure_values = (Value*)(func_value + 1);
					for(int i = 0; i < func_value->num_closure_values; i++){
						if(findAt(closure_values[i])){
							return true;
						}
					}
					if(func_value->name && findAt(func_value->name)){
						return true;
					}
					break;
				}

			case OS_VALUE_TYPE_WEAKREF:
				break;

			default:
				OS_ASSERT(false);
			}
			return false;
		}

	} lib = {this, val};

	if(lib.findAt(check_recursion)){
		return true;
	}
	if(lib.findAt(global_vars)){
		return true;
	}
	if(lib.findAt(user_pool)){
		return true;
	}
	int i;
	for(i = 0; i < PROTOTYPE_COUNT; i++){
		if(lib.findAt(prototypes[i])){
			return true;
		}
	}
	for(i = 0; i < stack_values.count; i++){
		if(lib.findAt(stack_values[i])){
			return true;
		}
	}
	for(i = 0; i < call_stack_funcs.count; i++){
		if(lib.findAt(&call_stack_funcs[i])){
			return true;
		}
	}
	return false;
}
#endif

void OS::Core::deleteValue(GCValue * val)
{
	OS_ASSERT(val);
	OS_ASSERT(val->gc_color != GC_GREY);
	unregisterValue(val->value_id);
	clearValue(val);
	val->~GCValue();
	free(val);
	num_destroyed_values++;
}

OS::Core::Property * OS::Core::setTableValue(Table * table, const PropertyIndex& index, Value value)
{
	OS_ASSERT(table);

	// TODO: correct ???
	gcAddToGreyList(value);

	Property * prop = table->get(index);
	if(prop){
		prop->value = value;
		return prop;
	}
	prop = new (malloc(sizeof(Property) OS_DBG_FILEPOS)) Property(index);
	prop->value = value;
	addTableProperty(table, prop);
	return prop;
}

bool OS::Core::hasSpecialPrefix(const Value& value)
{
	if(value.type != OS_VALUE_TYPE_STRING){
		return false;
	}
	OS_ASSERT(dynamic_cast<GCStringValue*>(value.v.string));
	GCStringValue * string = value.v.string;
	if(string->getLen() >= 2){
		const OS_CHAR * s = string->toChar();
		return s[0] == OS_TEXT('_') && s[1] == OS_TEXT('_');
	}
	return false;
}

void OS::Core::setPropertyValue(GCValue * table_value, const PropertyIndex& index, Value value, bool anonymous_setter_enabled, bool named_setter_enabled)
{
#if defined OS_DEBUG && defined OS_WARN_NULL_INDEX
	if(table_value != check_recursion && index.index.type == OS_VALUE_TYPE_NULL){
		error(OS_E_WARNING, OS_TEXT("object set null index"));
	}
#endif
	// TODO: correct ???
	gcAddToGreyList(value);

	if(index.index.type == OS_VALUE_TYPE_STRING){
		OS_ASSERT(dynamic_cast<GCStringValue*>(index.index.v.string));
		switch(value.type){
		case OS_VALUE_TYPE_FUNCTION:
			OS_ASSERT(dynamic_cast<GCFunctionValue*>(value.v.func));
			if(!value.v.func->name){
				value.v.func->name = index.index.v.string;
			}
			break;

		case OS_VALUE_TYPE_CFUNCTION:
			OS_ASSERT(dynamic_cast<GCCFunctionValue*>(value.v.cfunc));
			if(!value.v.cfunc->name){
				value.v.cfunc->name = index.index.v.string;
			}
			break;
		}
	}

	Property * prop = NULL;
	Table * table = table_value->table;
	if(table && (prop = table->get(index))){
		prop->value = value;
		return;
	}

	// prototype should not be used in set
	/* if(prototype_enabled){
	GCValue * cur_value = table_value;
	while(cur_value->prototype){
	cur_value = cur_value->prototype;
	Table * cur_table = cur_value->table;
	if(cur_table && (prop = cur_table->get(index))){
	prop->value = value;
	return;
	}
	}
	} */

	if(index.index.type == OS_VALUE_TYPE_STRING && strings->syntax_prototype == index.index.v.string){
		switch(table_value->type){
		case OS_VALUE_TYPE_STRING:
		case OS_VALUE_TYPE_ARRAY:
		case OS_VALUE_TYPE_OBJECT:
		case OS_VALUE_TYPE_FUNCTION:
			table_value->prototype = value.v.value;
			break;

		case OS_VALUE_TYPE_USERDATA:
		case OS_VALUE_TYPE_USERPTR:
		case OS_VALUE_TYPE_CFUNCTION:
			// TODO: warning???
			break;
		}
		return;
	}

	if(table_value->type == OS_VALUE_TYPE_ARRAY){
		OS_ASSERT(dynamic_cast<GCArrayValue*>(table_value));
		GCArrayValue * arr = (GCArrayValue*)table_value;
		int i = (int)valueToInt(index.index);
		if(i < 0) i += arr->values.count;
		if(i >= 0){
			while(i >= arr->values.count){
				allocator->vectorAddItem(arr->values, Value() OS_DBG_FILEPOS);
			}
			OS_ASSERT(i < arr->values.count);
			arr->values[i] = value;
		}
		return;
	}

	if((anonymous_setter_enabled || named_setter_enabled) && !hasSpecialPrefix(index.index)){
		Value func;
		if(index.index.type == OS_VALUE_TYPE_STRING && named_setter_enabled){
			const void * buf1 = strings->__setAt.toChar();
			int size1 = strings->__setAt.getDataSize();
			const void * buf2 = index.index.v.string->toChar();
			int size2 = index.index.v.string->getDataSize();
			GCStringValue * setter_name = newStringValue(buf1, size1, buf2, size2);
			if(getPropertyValue(func, table_value, PropertyIndex(setter_name, PropertyIndex::KeepStringIndex()), true)){
				pushValue(func);
				pushValue(table_value);
				pushValue(value);
				call(1, 0);
				return;
			}
		}
		if(anonymous_setter_enabled && getPropertyValue(func, table_value, PropertyIndex(strings->__set, PropertyIndex::KeepStringIndex()), true)){
			pushValue(func);
			pushValue(table_value);
			pushValue(index.index);
			pushValue(value);
			call(2, 0);
			return;
		}
	}
	if(table_value->type == OS_VALUE_TYPE_STRING){
		// TODO: trigger error???
		return;
	}
	if(!table){
		table_value->table = table = newTable(OS_DBG_FILEPOS_START);
	}
	prop = new (malloc(sizeof(Property) OS_DBG_FILEPOS)) Property(index);
	prop->value = value;
	addTableProperty(table, prop);
	// setTableValue(table, index, value);
}

void OS::Core::setPropertyValue(Value table_value, const PropertyIndex& index, Value value, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	switch(table_value.type){
	case OS_VALUE_TYPE_NULL:
		return;

	case OS_VALUE_TYPE_BOOL:
		// return setPropertyValue(prototypes[PROTOTYPE_BOOL], index, value, setter_enabled);
		return;

	case OS_VALUE_TYPE_NUMBER:
		// return setPropertyValue(prototypes[PROTOTYPE_NUMBER], index, value, setter_enabled);
		return;

	case OS_VALUE_TYPE_STRING:
		// return setPropertyValue(prototypes[PROTOTYPE_STRING], index, value, setter_enabled);
		// return;

	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		return setPropertyValue(table_value.v.value, index, value, anonymous_setter_enabled, named_setter_enabled);
	}
}

void OS::Core::pushPrototype(Value val)
{
	switch(val.type){
	case OS_VALUE_TYPE_NULL:
		pushNull();
		return;

	case OS_VALUE_TYPE_BOOL:
		pushValue(prototypes[PROTOTYPE_BOOL]);
		return;

	case OS_VALUE_TYPE_NUMBER:
		pushValue(prototypes[PROTOTYPE_NUMBER]);
		return;

	case OS_VALUE_TYPE_STRING:
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		pushValue(val.v.value);
		return;
	}
}

void OS::Core::setPrototype(Value val, Value proto, int userdata_crc)
{
	switch(val.type){
	case OS_VALUE_TYPE_NULL:
	case OS_VALUE_TYPE_BOOL:
	case OS_VALUE_TYPE_NUMBER:
		return;

	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		if(val.v.userdata->crc != userdata_crc){
			return;
		}
		// no break

	case OS_VALUE_TYPE_STRING:
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		val.v.value->prototype = proto.getGCValue();
		return;
	}
}

OS::Core::GCStringValue * OS::Core::newStringValue(const OS_CHAR * str)
{
	return newStringValue(str, OS_STRLEN(str));
}

OS::Core::GCStringValue * OS::Core::newStringValue(const OS_CHAR * str, int len)
{
	return newStringValue((void*)str, len * sizeof(OS_CHAR));
}

OS::Core::GCStringValue * OS::Core::newStringValue(const OS_CHAR * str, int len, const OS_CHAR * str2, int len2)
{
	return newStringValue((void*)str, len * sizeof(OS_CHAR), str2, len2 * sizeof(OS_CHAR));
}

OS::Core::GCStringValue * OS::Core::newStringValue(const OS_CHAR * str, int len, bool trim_left, bool trim_right)
{
	if(trim_left){
		while(len > 0 && OS_IS_SPACE(*str)){
			str++;
			len--;
		}
	}
	if(trim_right){
		while(len > 0 && OS_IS_SPACE(str[len-1])){
			len--;
		}
	}
	return newStringValue((void*)str, len * sizeof(OS_CHAR));
}

OS::Core::GCStringValue * OS::Core::newStringValue(const String& p_str, bool trim_left, bool trim_right)
{
	const OS_CHAR * str = p_str.toChar();
	int len = p_str.getLen();
	bool changed = false;
	if(trim_left){
		while(len > 0 && OS_IS_SPACE(*str)){
			str++;
			len--;
			changed = true;
		}
	}
	if(trim_right){
		while(len > 0 && OS_IS_SPACE(str[len-1])){
			len--;
			changed = true;
		}
	}
	if(changed){
		return newStringValue(str, len);
	}
	return p_str.string;
}

OS::Core::GCStringValue * OS::Core::newStringValue(const String& str)
{
	return str.string;
}

OS::Core::GCStringValue * OS::Core::newStringValue(const void * buf, int size)
{
	return newStringValue(buf, size, NULL, 0);
}

OS::Core::GCStringValue * OS::Core::newStringValue(const void * buf1, int size1, const void * buf2, int size2)
{
	if(string_refs.count > 0){
		OS_ASSERT(string_refs.heads && string_refs.head_mask);
		int hash = Utils::keyToHash(buf1, size1, buf2, size2);
		int slot = hash & string_refs.head_mask;
		StringRef * str_ref = string_refs.heads[slot];
		for(StringRef * prev = NULL, * next; str_ref; str_ref = next){
			next = str_ref->hash_next;
			GCStringValue * string_value = (GCStringValue*)values.get(str_ref->string_value_id);
			if(!string_value){
				if(!prev){
					string_refs.heads[slot] = next;
				}else{
					prev->hash_next = next;					
				}
				free(str_ref);
				string_refs.count--;
				continue;
			}
			OS_ASSERT(string_value->type == OS_VALUE_TYPE_STRING);
			OS_ASSERT(dynamic_cast<GCStringValue*>(string_value));
			if(string_value->isEqual(hash, buf1, size1, buf2, size2)){
				return string_value;
			}
			prev = str_ref;
		}
	}
	GCStringValue * string_value = GCStringValue::alloc(allocator, buf1, size1, buf2, size2 OS_DBG_FILEPOS);
	StringRef * str_ref = (StringRef*)malloc(sizeof(StringRef) OS_DBG_FILEPOS);
	str_ref->string_hash = string_value->hash;
	str_ref->string_value_id = string_value->value_id;
	str_ref->hash_next = NULL;
	registerStringRef(str_ref);
	return string_value;
}

OS::Core::GCStringValue * OS::Core::newStringValue(const void * buf1, int size1, const void * buf2, int size2, const void * buf3, int size3)
{
	if(size1 <= 0){
		return newStringValue(buf2, size2, buf3, size3);
	}
	if(size2 <= 0){
		return newStringValue(buf1, size1, buf3, size3);
	}
	if(size3 <= 0){
		return newStringValue(buf1, size1, buf2, size2);
	}
	if(size1 + size2 + size3 <= 1024){
		OS_BYTE * buf = (OS_BYTE*)alloca(size1 + size2 + size3 + sizeof(OS_CHAR));
		OS_MEMCPY(buf, buf1, size1);
		OS_MEMCPY(buf+size1, buf2, size2);
		OS_MEMCPY(buf+size1+size2, buf3, size3);
		buf[size1+size2+size3] = (OS_CHAR)0;
		return newStringValue(buf, (size1 + size2 + size3) / sizeof(OS_CHAR));
	}
	GCStringValue * str = newStringValue(buf1, size1, buf2, size2);
	return newStringValue(str->toBytes(), str->data_size, buf3, size3);
}

OS::Core::GCStringValue * OS::Core::newStringValue(GCStringValue * a, GCStringValue * b)
{
	if(a->data_size <= 0){
		return b;
	}
	if(b->data_size <= 0){
		return a;
	}
	return newStringValue((void*)a->toBytes(), a->data_size, (void*)b->toBytes(), b->data_size);
}

OS::Core::GCStringValue * OS::Core::newStringValue(const String& a, const String& b)
{
	return newStringValue(a.string, b.string);
}

OS::Core::GCStringValue * OS::Core::newStringValue(OS_INT val)
{
	OS_CHAR str[128];
	Utils::numToStr(str, val);
	return newStringValue(str);
}

OS::Core::GCStringValue * OS::Core::newStringValue(OS_FLOAT val, int precision)
{
	OS_CHAR str[128];
	Utils::numToStr(str, val, precision);
	return newStringValue(str);
}

OS::Core::GCStringValue * OS::Core::newStringValue(int temp_buf_len, const OS_CHAR * fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	OS_VaListDtor va_dtor(&va);
	return newStringValueVa(temp_buf_len, fmt, va);
}

OS::Core::GCStringValue * OS::Core::newStringValueVa(int temp_buf_len, const OS_CHAR * fmt, va_list va)
{
	OS_ASSERT(temp_buf_len <= OS_DEF_FMT_BUF_LEN);
	OS_CHAR * buf = (OS_CHAR*)malloc(temp_buf_len * sizeof(OS_CHAR) OS_DBG_FILEPOS);
	OS_VSNPRINTF(buf, sizeof(OS_CHAR) * (temp_buf_len-1), fmt, va);
	GCStringValue * result = newStringValue(buf);
	free(buf);
	return result;
}

OS::Core::GCCFunctionValue * OS::Core::newCFunctionValue(OS_CFunction func, void * user_param)
{
	return newCFunctionValue(func, 0, user_param);
}

OS::Core::GCCFunctionValue * OS::Core::newCFunctionValue(OS_CFunction func, int num_closure_values, void * user_param)
{
	OS_ASSERT(stack_values.count >= num_closure_values);
	if(!func){
		return NULL;
	}
	GCCFunctionValue * res = new (malloc(sizeof(GCCFunctionValue) + sizeof(Value) * num_closure_values OS_DBG_FILEPOS)) GCCFunctionValue();
	res->prototype = prototypes[PROTOTYPE_FUNCTION];
	res->name = NULL;
	res->func = func;
	res->user_param = user_param;
	res->num_closure_values = num_closure_values;
	Value * closure_values = (Value*)(res + 1);
	if(num_closure_values > 0){
		OS_MEMCPY(closure_values, stack_values.buf + (stack_values.count - num_closure_values), sizeof(Value)*num_closure_values);
	}
	res->type = OS_VALUE_TYPE_CFUNCTION;
	pop(num_closure_values);
	registerValue(res);
	return res;
}

OS::Core::GCUserdataValue * OS::Core::newUserdataValue(int crc, int data_size, OS_UserdataDtor dtor, void * user_param)
{
	GCUserdataValue * res = new (malloc(sizeof(GCUserdataValue) + data_size OS_DBG_FILEPOS)) GCUserdataValue();
	res->prototype = prototypes[PROTOTYPE_USERDATA];
	res->crc = crc;
	res->dtor = dtor;
	res->user_param = user_param;
	res->ptr = data_size ? res + 1 : NULL;
	res->type = OS_VALUE_TYPE_USERDATA;
	registerValue(res);
	return res;
}

OS::Core::GCUserdataValue * OS::Core::newUserPointerValue(int crc, void * ptr, OS_UserdataDtor dtor, void * user_param)
{
	int hash = (int)(intptr_t)ptr;
	if(userptr_refs.count > 0){
		OS_ASSERT(userptr_refs.heads && userptr_refs.head_mask);
		int slot = hash & userptr_refs.head_mask;
		UserptrRef * userptr_ref = userptr_refs.heads[slot];
		for(UserptrRef * prev = NULL, * next; userptr_ref; userptr_ref = next){
			next = userptr_ref->hash_next;
			GCUserdataValue * userptr_value = (GCUserdataValue*)values.get(userptr_ref->userptr_value_id);
			if(!userptr_value){
				if(!prev){
					userptr_refs.heads[slot] = next;
				}else{
					prev->hash_next = next;					
				}
				free(userptr_ref);
				userptr_refs.count--;
				continue;
			}
			OS_ASSERT(userptr_value->type == OS_VALUE_TYPE_USERPTR);
			OS_ASSERT(dynamic_cast<GCUserdataValue*>(userptr_value));
			if(userptr_value->ptr == ptr){ // && userptr_value->crc == crc){
				OS_ASSERT(userptr_value->crc == crc);
				if(userptr_value->crc != crc){
					if(!prev){
						userptr_refs.heads[slot] = next;
					}else{
						prev->hash_next = next;					
					}
					free(userptr_ref);
					userptr_refs.count--;
					continue;
				}
				return userptr_value;
			}
			prev = userptr_ref;
		}
	}
	GCUserdataValue * res = new (malloc(sizeof(GCUserdataValue) OS_DBG_FILEPOS)) GCUserdataValue();
	res->prototype = prototypes[PROTOTYPE_USERDATA];
	res->crc = crc;
	res->dtor = dtor;
	res->user_param = user_param;
	res->ptr = ptr;
	res->type = OS_VALUE_TYPE_USERPTR;
	registerValue(res);

	UserptrRef * userptr_ref = (UserptrRef*)malloc(sizeof(UserptrRef) OS_DBG_FILEPOS);
	userptr_ref->userptr_hash = hash;
	userptr_ref->userptr_value_id = res->value_id;
	userptr_ref->hash_next = NULL;
	registerUserptrRef(userptr_ref);

	return res;
}

OS::Core::GCObjectValue * OS::Core::newObjectValue()
{
	return newObjectValue(prototypes[PROTOTYPE_OBJECT]);
}

OS::Core::GCObjectValue * OS::Core::newObjectValue(GCValue * prototype)
{
	GCObjectValue * res = new (malloc(sizeof(GCObjectValue) OS_DBG_FILEPOS)) GCObjectValue();
	res->prototype = prototype;
	res->type = OS_VALUE_TYPE_OBJECT;
	registerValue(res);
	return res;
}

OS::Core::GCArrayValue * OS::Core::newArrayValue(int initial_capacity)
{
	GCArrayValue * res = new (malloc(sizeof(GCArrayValue) OS_DBG_FILEPOS)) GCArrayValue();
	res->prototype = prototypes[PROTOTYPE_ARRAY];
	res->type = OS_VALUE_TYPE_ARRAY;
	if(initial_capacity > 0){
		allocator->vectorReserveCapacity(res->values, initial_capacity OS_DBG_FILEPOS);
	}
	registerValue(res);
	return res;
}

void OS::Core::pushValue(const Value& p_val)
{
	StackValues& stack_values = this->stack_values;
	if(stack_values.capacity < stack_values.count+1){
		Value val = p_val;
		reserveStackValues(stack_values.count+1);
		stack_values.buf[stack_values.count++] = val;
	}else{
		stack_values.buf[stack_values.count++] = p_val;
	}
}

void OS::Core::pushNull()
{
	pushValue(Value());
}

void OS::Core::pushStackValue(int offs)
{
	pushValue(getStackValue(offs));
}

void OS::Core::copyValue(int raw_from, int raw_to)
{
	reserveStackValues(raw_to+1);
	stack_values.buf[raw_to] = stack_values.buf[raw_from];
}
/*
void OS::Core::pushTrue()
{
	pushValue(true);
}

void OS::Core::pushFalse()
{
	pushValue(false);
}
*/
void OS::Core::pushBool(bool val)
{
#if 1 // speed optimization
	StackValues& stack_values = this->stack_values;
	if(stack_values.capacity < stack_values.count+1){
		reserveStackValues(stack_values.count+1);
	}
	stack_values.buf[stack_values.count++] = val;
#else
	pushValue(val);
#endif
}

void OS::Core::pushNumber(OS_INT32 val)
{
#if 1 // speed optimization
	StackValues& stack_values = this->stack_values;
	if(stack_values.capacity < stack_values.count+1){
		reserveStackValues(stack_values.count+1);
	}
	stack_values.buf[stack_values.count++] = val;
#else
	pushValue(val);
#endif
}

void OS::Core::pushNumber(OS_INT64 val)
{
#if 1 // speed optimization
	StackValues& stack_values = this->stack_values;
	if(stack_values.capacity < stack_values.count+1){
		reserveStackValues(stack_values.count+1);
	}
	stack_values.buf[stack_values.count++] = val;
#else
	pushValue(val);
#endif
}

void OS::Core::pushNumber(float val)
{
#if 1 // speed optimization
	StackValues& stack_values = this->stack_values;
	if(stack_values.capacity < stack_values.count+1){
		reserveStackValues(stack_values.count+1);
	}
	stack_values.buf[stack_values.count++] = val;
#else
	pushValue(val);
#endif
}

void OS::Core::pushNumber(double val)
{
#if 1 // speed optimization
	StackValues& stack_values = this->stack_values;
	if(stack_values.capacity < stack_values.count+1){
		reserveStackValues(stack_values.count+1);
	}
	stack_values.buf[stack_values.count++] = val;
#else
	pushValue(val);
#endif
}

OS::Core::GCStringValue * OS::Core::pushStringValue(const String& val)
{
	return pushValue(newStringValue(val));
}

OS::Core::GCStringValue * OS::Core::pushStringValue(const OS_CHAR * val)
{
	return pushValue(newStringValue(val));
}

OS::Core::GCStringValue * OS::Core::pushStringValue(const OS_CHAR * val, int len)
{
	return pushValue(newStringValue(val, len));
}

OS::Core::GCCFunctionValue * OS::Core::pushCFunctionValue(OS_CFunction func, void * user_param)
{
	return pushValue(newCFunctionValue(func, user_param));
}

OS::Core::GCCFunctionValue * OS::Core::pushCFunctionValue(OS_CFunction func, int closure_values, void * user_param)
{
	return pushValue(newCFunctionValue(func, closure_values, user_param));
}

OS::Core::GCUserdataValue * OS::Core::pushUserdataValue(int crc, int data_size, OS_UserdataDtor dtor, void * user_param)
{
	return pushValue(newUserdataValue(crc, data_size, dtor, user_param));
}

OS::Core::GCUserdataValue * OS::Core::pushUserPointerValue(int crc, void * data, OS_UserdataDtor dtor, void * user_param)
{
	return pushValue(newUserPointerValue(crc, data, dtor, user_param));
}

OS::Core::GCObjectValue * OS::Core::pushObjectValue()
{
	return pushValue(newObjectValue());
}

OS::Core::GCObjectValue * OS::Core::pushObjectValue(GCValue * prototype)
{
	return pushValue(newObjectValue(prototype));
}

OS::Core::GCArrayValue * OS::Core::pushArrayValue(int initial_capacity)
{
	return pushValue(newArrayValue(initial_capacity));
}

void OS::Core::pushTypeOf(Value val)
{
	switch(val.type){
		// case OS_VALUE_TYPE_NULL:
	case OS_VALUE_TYPE_BOOL:
		pushStringValue(strings->typeof_boolean);
		return;

	case OS_VALUE_TYPE_NUMBER:
		pushStringValue(strings->typeof_number);
		return;

	case OS_VALUE_TYPE_STRING:
		pushStringValue(strings->typeof_string);
		return;

	case OS_VALUE_TYPE_ARRAY:
		pushStringValue(strings->typeof_array);
		return;

	case OS_VALUE_TYPE_OBJECT:
		pushStringValue(strings->typeof_object);
		return;

	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		pushStringValue(strings->typeof_userdata);
		return;

	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		pushStringValue(strings->typeof_function);
		return;
	}
	pushStringValue(strings->typeof_null);
}

bool OS::Core::pushNumberOf(Value val)
{
	if(val.type == OS_VALUE_TYPE_NUMBER){
		pushValue(val);
		return true;
	}
	OS_NUMBER number;
	if(isValueNumber(val, &number)){
		pushNumber(number);
		return true;
	}
	pushNull();
	return false;
}

bool OS::Core::pushStringOf(Value val)
{
	if(val.type == OS_VALUE_TYPE_STRING){
		pushValue(val);
		return true;
	}
	String str(allocator);
	if(isValueString(val, &str)){
		pushStringValue(str);
		return true;
	}
	pushNull();
	return false;
}

bool OS::Core::pushValueOf(Value val)
{
	switch(val.type){
	case OS_VALUE_TYPE_NULL:
	case OS_VALUE_TYPE_NUMBER:
	case OS_VALUE_TYPE_BOOL:
	case OS_VALUE_TYPE_STRING:
		pushValue(val);
		return true;
	}

	OS_ASSERT(check_recursion && check_recursion->type == OS_VALUE_TYPE_OBJECT);
	if(++check_recursion->external_ref_count == 1 && check_recursion->table){
		clearTable(check_recursion->table);
	}
	setPropertyValue(check_recursion, val, Value(true), false, false);
	struct Finalizer { 
		Core * core; 
		~Finalizer()
		{ 
			if(--core->check_recursion->external_ref_count == 0 && core->check_recursion->table){
				core->clearTable(core->check_recursion->table);
			}
			if(core->check_recursion->gc_color == GC_WHITE){
				core->check_recursion->gc_color = GC_BLACK;
			}
		}
	} finalizer = {this};

	bool prototype_enabled = true;
	Value func;
	if(getPropertyValue(func, val.v.value, PropertyIndex(strings->__valueof, PropertyIndex::KeepStringIndex()), prototype_enabled)
		&& func.isFunction())
	{
		pushValue(func);
		pushValue(val);
		call(0, 1);
		switch(stack_values.lastElement().type){
		case OS_VALUE_TYPE_NULL:
		case OS_VALUE_TYPE_NUMBER:
		case OS_VALUE_TYPE_BOOL:
		case OS_VALUE_TYPE_STRING:
			return true;
		}
		// TODO: warning
		pop();
	}
	pushNull();
	return false;
}

OS::Core::GCArrayValue * OS::Core::pushArrayOf(Value val)
{
	// GCArrayValue * arr;
	switch(val.type){
		// case OS_VALUE_TYPE_NULL:
		// 	return pushNull(); // pushArrayValue();

		/*
		case OS_VALUE_TYPE_BOOL:
		case OS_VALUE_TYPE_NUMBER:
		case OS_VALUE_TYPE_STRING:
		arr = pushArrayValue();
		allocator->vectorAddItem(arr->values, val OS_DBG_FILEPOS);
		return arr;
		*/

	case OS_VALUE_TYPE_ARRAY:
		return pushValue(val.v.arr);

		/*
		case OS_VALUE_TYPE_OBJECT:
		arr = pushArrayValue();
		if(val.v.object->table && val.v.object->table->count > 0){
		Property * prop = val.v.object->table->first;
		for(; prop; prop = prop->next){
		allocator->vectorAddItem(arr->values, prop->value OS_DBG_FILEPOS);
		}
		}
		return arr;
		*/
	}
	pushNull();
	return NULL;
}

OS::Core::GCObjectValue * OS::Core::pushObjectOf(Value val)
{
	// GCObjectValue * object;
	switch(val.type){
		// case OS_VALUE_TYPE_NULL:
		// 	return pushObjectValue();

		/*
		case OS_VALUE_TYPE_BOOL:
		case OS_VALUE_TYPE_NUMBER:
		case OS_VALUE_TYPE_STRING:
		object = pushObjectValue();
		setPropertyValue(object, Value(0), val, false);
		return object;

		case OS_VALUE_TYPE_ARRAY:
		{
		OS_ASSERT(dynamic_cast<GCArrayValue*>(val.v.arr));
		object = pushObjectValue();
		GCArrayValue * arr = (GCArrayValue*)val.v.arr;
		for(int i = 0; i < arr->values.count; i++){
		setPropertyValue(object, Value(i), arr->values[i], false);
		}
		return object;
		}
		*/

	case OS_VALUE_TYPE_OBJECT:
		return pushValue(val.v.object);
	}
	pushNull();
	return NULL;
}

OS::Core::GCUserdataValue * OS::Core::pushUserdataOf(Value val)
{
	switch(val.type){
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		return pushValue(val.v.userdata);
	}
	pushNull();
	return NULL;
}

bool OS::Core::pushFunctionOf(Value val)
{
	switch(val.type){
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		pushValue(val);
		return true;
	}
	pushNull();
	return false;
}

void OS::Core::pushCloneValue(Value val)
{
	GCValue * value, * new_value;
	switch(val.type){
	case OS_VALUE_TYPE_NULL:
	case OS_VALUE_TYPE_BOOL:
	case OS_VALUE_TYPE_NUMBER:
	case OS_VALUE_TYPE_STRING:
		pushValue(val);
		return;

	case OS_VALUE_TYPE_ARRAY:
		{
			OS_ASSERT(dynamic_cast<GCArrayValue*>(val.v.value));
			value = val.v.value;
			new_value = pushArrayValue();
			new_value->prototype = value->prototype;
			GCArrayValue * arr = (GCArrayValue*)value;
			GCArrayValue * new_arr = (GCArrayValue*)new_value;
			allocator->vectorReserveCapacity(new_arr->values, arr->values.count OS_DBG_FILEPOS);
			for(int i = 0; i < arr->values.count; i++){
				allocator->vectorAddItem(new_arr->values, arr->values[i] OS_DBG_FILEPOS);
			}
			break;
		}

	case OS_VALUE_TYPE_OBJECT:
		value = val.v.value;
		new_value = pushObjectValue(value->prototype);
		break;

	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_CFUNCTION:
		value = val.v.value;
		new_value = pushValue(value);
		break;

	default:
		pushNull();
		return;
	}
	OS_ASSERT(new_value->type != OS_VALUE_TYPE_NULL);
	if(new_value != value && value->table && value->table->count > 0){
		new_value->table = newTable(OS_DBG_FILEPOS_START);
		copyTableProperties(new_value->table, value->table);
	}
	// removeStackValue(-2);

	switch(new_value->type){
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		{
			bool prototype_enabled = true;
			Value func;
			if(getPropertyValue(func, new_value, 
				PropertyIndex(strings->__clone, PropertyIndex::KeepStringIndex()), prototype_enabled)
				&& func.isFunction())
			{
				pushValue(func);
				pushValue(new_value);
				call(0, 1);
				OS_ASSERT(stack_values.count >= 2);
				removeStackValue(-2);
			}
		}
	}
}

void OS::Core::pushOpResultValue(int opcode, Value value)
{
	struct Lib
	{
		Core * core;

		void pushSimpleOpcodeValue(int opcode, Value value)
		{
			switch(opcode){
			case Program::OP_BIT_NOT:
				return core->pushNumber(~core->valueToInt(value));

			case Program::OP_PLUS:
				if(value.type == OS_VALUE_TYPE_NUMBER){
					return core->pushValue(value);
				}
				return core->pushNumber(core->valueToNumber(value));

			case Program::OP_NEG:
				return core->pushNumber(-core->valueToNumber(value));

			case Program::OP_LENGTH:
				// return core->pushNumber(core->valueToString(value).getDataSize() / sizeof(OS_CHAR));
				return pushObjectMethodOpcodeValue(core->strings->__len, value);
			}
			return core->pushNull();
		}

		void pushObjectMethodOpcodeValue(const String& method_name, Value value)
		{
			bool prototype_enabled = true;
			Value func;
			if(core->getPropertyValue(func, value, 
				PropertyIndex(method_name, PropertyIndex::KeepStringIndex()), prototype_enabled)
				&& func.isFunction())
			{
				core->pushValue(func);
				core->pushValue(value);
				core->call(0, 1);
				return;
			}
			return core->pushNull();
		}

		void pushObjectOpcodeValue(int opcode, Value value)
		{
			switch(opcode){
			case Program::OP_BIT_NOT:
				return pushObjectMethodOpcodeValue(core->strings->__bitnot, value);

			case Program::OP_PLUS:
				return pushObjectMethodOpcodeValue(core->strings->__plus, value);

			case Program::OP_NEG:
				return pushObjectMethodOpcodeValue(core->strings->__neg, value);

			case Program::OP_LENGTH:
				return pushObjectMethodOpcodeValue(core->strings->__len, value);
			}
			return core->pushNull();
		}

		void pushUnaryOpcodeValue(int opcode, Value value)
		{
			switch(value.type){
			case OS_VALUE_TYPE_NULL:
			case OS_VALUE_TYPE_NUMBER:
			case OS_VALUE_TYPE_BOOL:
			case OS_VALUE_TYPE_STRING:
				return pushSimpleOpcodeValue(opcode, value);

			case OS_VALUE_TYPE_ARRAY:
			case OS_VALUE_TYPE_OBJECT:
			case OS_VALUE_TYPE_USERDATA:
			case OS_VALUE_TYPE_USERPTR:
				return pushObjectOpcodeValue(opcode, value);
			}
			return core->pushNull();
		}
	} lib = {this};
	return lib.pushUnaryOpcodeValue(opcode, value);
}

void OS::Core::pushOpResultValue(int opcode, Value left_value, Value right_value)
{
	struct Lib
	{
		Core * core;

		bool isEqualExactly(Value left_value, Value right_value)
		{
			if(left_value.type == right_value.type){ // && left_value->prototype == right_value->prototype){
				switch(left_value.type){
				case OS_VALUE_TYPE_NULL:
					return true;

				case OS_VALUE_TYPE_NUMBER:
					return left_value.v.number == right_value.v.number;

				case OS_VALUE_TYPE_BOOL:
					return left_value.v.boolean == right_value.v.boolean;

				case OS_VALUE_TYPE_STRING:
					// the same strings are always share one instance, so check only gc value ptr

				case OS_VALUE_TYPE_ARRAY:
				case OS_VALUE_TYPE_OBJECT:
				case OS_VALUE_TYPE_USERDATA:
				case OS_VALUE_TYPE_USERPTR:
				case OS_VALUE_TYPE_FUNCTION:
				case OS_VALUE_TYPE_CFUNCTION:
					return left_value.v.value == right_value.v.value;

				case OS_VALUE_TYPE_WEAKREF:
					return left_value.v.value_id == right_value.v.value_id;
				}
			}
			return false;
		}

		int compareNumbers(OS_NUMBER num1, OS_NUMBER num2)
		{
			if(num1 > num2){
				return 1;
			}
			if(num1 < num2){
				return -1;
			}
			return 0;
		}

		int compareStringAndNumber(GCStringValue * left_string_data, OS_NUMBER right_number)
		{
#if 1
			return compareNumbers(left_string_data->toNumber(), right_number);
#else
			OS_CHAR buf[128];
			Utils::numToStr(buf, right_number);
			return left_string_data->cmp(buf);
#endif
		}

		int compareStrings(GCStringValue * left_string_data, GCStringValue * right_string_data)
		{
			return left_string_data->cmp(right_string_data);
		}

		int compareObjectAndValue(Value left_value, Value right_value)
		{
			GCValue * left = left_value.v.value;
			switch(left->type){
			case OS_VALUE_TYPE_STRING:
				{
					OS_ASSERT(dynamic_cast<GCStringValue*>(left));
					GCStringValue * string = (GCStringValue*)left;
					return compareStringAndValue(left_value, string, right_value);
				}

			case OS_VALUE_TYPE_ARRAY:
			case OS_VALUE_TYPE_OBJECT:
			case OS_VALUE_TYPE_USERDATA:
			case OS_VALUE_TYPE_USERPTR:
				switch(right_value.type){
				case OS_VALUE_TYPE_NULL:
					return 1;

				case OS_VALUE_TYPE_STRING:
				case OS_VALUE_TYPE_NUMBER:
				case OS_VALUE_TYPE_BOOL:
				case OS_VALUE_TYPE_ARRAY:
				case OS_VALUE_TYPE_OBJECT:
				case OS_VALUE_TYPE_USERDATA:
				case OS_VALUE_TYPE_USERPTR:
					{
						bool prototype_enabled = true;
						Value func;
						if(core->getPropertyValue(func, left, 
							PropertyIndex(core->strings->__cmp, PropertyIndex::KeepStringIndex()), prototype_enabled)
							&& func.isFunction())
						{
							core->pushValue(func);
							core->pushValue(left);
							core->pushValue(right_value);
							core->call(1, 1);
							OS_ASSERT(core->stack_values.count >= 1);
							struct Pop { Core * core; ~Pop(){ core->pop(); } } pop = {core};
							Value value = core->stack_values.lastElement();
							if(value.type == OS_VALUE_TYPE_NUMBER){
								return (int)value.v.number;
							}
						}
						if(right_value.type != OS_VALUE_TYPE_STRING && right_value.type != OS_VALUE_TYPE_NUMBER && right_value.type != OS_VALUE_TYPE_BOOL){
							GCValue * right = right_value.v.value;
							OS_ASSERT(right->type == right_value.type);
							if(left->prototype != right->prototype){
								switch(right->type){
								case OS_VALUE_TYPE_ARRAY:
								case OS_VALUE_TYPE_OBJECT:
								case OS_VALUE_TYPE_USERDATA:
								case OS_VALUE_TYPE_USERPTR:
									if(core->getPropertyValue(func, right, 
										PropertyIndex(core->strings->__cmp, PropertyIndex::KeepStringIndex()), prototype_enabled)
										&& func.isFunction())
									{
										core->pushValue(func);
										core->pushValue(right_value);
										core->pushValue(left);
										core->call(1, 1);
										OS_ASSERT(core->stack_values.count >= 1);
										struct Pop { Core * core; ~Pop(){ core->pop(); } } pop = {core};
										Value value = core->stack_values.lastElement();
										if(value.type == OS_VALUE_TYPE_NUMBER){
											return -(int)value.v.number;
										}
									}
								}
							}
						}
						core->pushValueOf(Value(left));
						Value left_value = core->stack_values.lastElement();
						struct Pop { Core * core; ~Pop(){ core->pop(); } } pop = {core};
						return compareValues(left_value, right_value);
					}
				}
				break;
			}
			// generic compare
			return 1; // left->value_id - (int)right_value;
		}

		int compareNumberToValue(Value left_value, OS_NUMBER left_number, Value right_value)
		{
			switch(right_value.type){
			case OS_VALUE_TYPE_NULL:
				return 1;

			case OS_VALUE_TYPE_NUMBER:
				return compareNumbers(left_number, right_value.v.number);

			case OS_VALUE_TYPE_BOOL:
				return compareNumbers(left_number, (OS_NUMBER)right_value.v.boolean);

			case OS_VALUE_TYPE_STRING:
				return -compareStringAndNumber(right_value.v.string, left_number);
			}
			return -compareObjectAndValue(right_value, left_value);
		}

		int compareStringAndValue(Value left_value, GCStringValue * left_string_data, Value right_value)
		{
			switch(right_value.type){
			case OS_VALUE_TYPE_NULL:
				return 1;

			case OS_VALUE_TYPE_NUMBER:
				return compareStringAndNumber(left_string_data, right_value.v.number);

			case OS_VALUE_TYPE_BOOL:
				return compareStringAndNumber(left_string_data, (OS_NUMBER)right_value.v.boolean);

			case OS_VALUE_TYPE_STRING:
				return compareStrings(left_string_data, right_value.v.string);
			}
			return -compareObjectAndValue(right_value, left_value);
		}

		int compareValues(Value left_value, Value right_value)
		{
			switch(left_value.type){
			case OS_VALUE_TYPE_NULL:
				return right_value.type == OS_VALUE_TYPE_NULL ? 0 : -1;

			case OS_VALUE_TYPE_NUMBER:
				return compareNumberToValue(left_value, left_value.v.number, right_value);

			case OS_VALUE_TYPE_BOOL:
				return compareNumberToValue(left_value, (OS_NUMBER)left_value.v.boolean, right_value);

				// case OS_VALUE_TYPE_STRING:
				// 	return compareStringAndValue(left_value->v.string_data, right_value);
			}
			return compareObjectAndValue(left_value, right_value);
		}

		void pushSimpleOpcodeValue(int opcode, Value left_value, Value right_value)
		{
			switch(opcode){
			case Program::OP_CONCAT:
				core->pushStringValue(core->newStringValue(core->valueToString(left_value), core->valueToString(right_value)));
				return;

			case Program::OP_BIT_AND:
				return core->pushNumber(core->valueToInt(left_value) & core->valueToInt(right_value));

			case Program::OP_BIT_OR:
				return core->pushNumber(core->valueToInt(left_value) | core->valueToInt(right_value));

			case Program::OP_BIT_XOR:
				return core->pushNumber(core->valueToInt(left_value) ^ core->valueToInt(right_value));

			case Program::OP_ADD: // +
				return core->pushNumber(core->valueToNumber(left_value) + core->valueToNumber(right_value));

			case Program::OP_SUB: // -
				return core->pushNumber(core->valueToNumber(left_value) - core->valueToNumber(right_value));

			case Program::OP_MUL: // *
				return core->pushNumber(core->valueToNumber(left_value) * core->valueToNumber(right_value));

			case Program::OP_DIV: // /
				{
					OS_FLOAT right = core->valueToNumber(right_value);
					if(!right){
						core->errorDivisionByZero();
						return core->pushNumber(0.0);
					}
					return core->pushNumber(core->valueToNumber(left_value) / right);
				}

			case Program::OP_MOD: // %
				{
					OS_FLOAT right = core->valueToNumber(right_value);
					if(!right){
						core->errorDivisionByZero();
						return core->pushNumber(0.0);
					}
					return core->pushNumber(OS_MATH_MOD_OPERATOR(core->valueToNumber(left_value), right));
				}

			case Program::OP_LSHIFT: // <<
				return core->pushNumber(core->valueToInt(left_value) << core->valueToInt(right_value));

			case Program::OP_RSHIFT: // >>
				return core->pushNumber(core->valueToInt(left_value) >> core->valueToInt(right_value));

			case Program::OP_POW: // **
				return core->pushNumber(OS_MATH_POW_OPERATOR(core->valueToNumber(left_value), core->valueToNumber(right_value)));
			}
			core->pushNull();
		}

		void pushObjectMethodOpcodeValue(int opcode, const String& method_name, Value left_value, Value right_value, GCValue * object, bool is_left_side)
		{
			bool prototype_enabled = true;
			Value func;
			if(core->getPropertyValue(func, object, 
				PropertyIndex(method_name, PropertyIndex::KeepStringIndex()), prototype_enabled)
				&& func.isFunction())
			{
				core->pushValue(func);
				core->pushValue(object);
				core->pushValue(left_value);
				core->pushValue(right_value);
				core->pushValue(is_left_side ? right_value : left_value);
				core->call(3, 1);
				return;
			}
			Value other_value = is_left_side ? right_value : left_value;
			switch(other_value.type){
			case OS_VALUE_TYPE_ARRAY:
			case OS_VALUE_TYPE_OBJECT:
			case OS_VALUE_TYPE_USERDATA:
			case OS_VALUE_TYPE_USERPTR:
				{
					GCValue * other = other_value.v.value;
					if(object->prototype == other->prototype){
						core->pushNull();
						return;
					}
					if(core->getPropertyValue(func, other, 
						PropertyIndex(method_name, PropertyIndex::KeepStringIndex()), prototype_enabled)
						&& func.isFunction())
					{
						core->pushValue(func);
						core->pushValue(other_value);
						core->pushValue(left_value);
						core->pushValue(right_value);
						core->pushValue(!is_left_side ? right_value : left_value);
						core->call(3, 1);
						return;
					}
				}
			}
			if(is_left_side){
				core->pushValueOf(left_value);
				pushBinaryOpcodeValue(opcode, core->stack_values.lastElement(), right_value);
				core->removeStackValue(-2);
			}else{
				core->pushValueOf(right_value);
				pushBinaryOpcodeValue(opcode, left_value, core->stack_values.lastElement());
				core->removeStackValue(-2);
			}
		}

		void pushObjectOpcodeValue(int opcode, Value left_value, Value right_value, GCValue * object, bool is_left_side)
		{
			switch(opcode){
			case Program::OP_CONCAT:
				return pushObjectMethodOpcodeValue(opcode, core->strings->__concat, left_value, right_value, object, is_left_side);

			case Program::OP_BIT_AND:
				return pushObjectMethodOpcodeValue(opcode, core->strings->__bitand, left_value, right_value, object, is_left_side);

			case Program::OP_BIT_OR:
				return pushObjectMethodOpcodeValue(opcode, core->strings->__bitor, left_value, right_value, object, is_left_side);

			case Program::OP_BIT_XOR:
				return pushObjectMethodOpcodeValue(opcode, core->strings->__bitxor, left_value, right_value, object, is_left_side);

			case Program::OP_ADD: // +
				return pushObjectMethodOpcodeValue(opcode, core->strings->__add, left_value, right_value, object, is_left_side);

			case Program::OP_SUB: // -
				return pushObjectMethodOpcodeValue(opcode, core->strings->__sub, left_value, right_value, object, is_left_side);

			case Program::OP_MUL: // *
				return pushObjectMethodOpcodeValue(opcode, core->strings->__mul, left_value, right_value, object, is_left_side);

			case Program::OP_DIV: // /
				return pushObjectMethodOpcodeValue(opcode, core->strings->__div, left_value, right_value, object, is_left_side);

			case Program::OP_MOD: // %
				return pushObjectMethodOpcodeValue(opcode, core->strings->__mod, left_value, right_value, object, is_left_side);

			case Program::OP_LSHIFT: // <<
				return pushObjectMethodOpcodeValue(opcode, core->strings->__lshift, left_value, right_value, object, is_left_side);

			case Program::OP_RSHIFT: // >>
				return pushObjectMethodOpcodeValue(opcode, core->strings->__rshift, left_value, right_value, object, is_left_side);

			case Program::OP_POW: // **
				return pushObjectMethodOpcodeValue(opcode, core->strings->__pow, left_value, right_value, object, is_left_side);
			}
			core->pushNull();
		}

		void pushBinaryOpcodeValue(int opcode, Value left_value, Value right_value)
		{
			switch(left_value.type){
			case OS_VALUE_TYPE_NULL:
			case OS_VALUE_TYPE_NUMBER:
			case OS_VALUE_TYPE_BOOL:
			case OS_VALUE_TYPE_STRING:
				switch(right_value.type){
				case OS_VALUE_TYPE_NULL:
				case OS_VALUE_TYPE_NUMBER:
				case OS_VALUE_TYPE_BOOL:
				case OS_VALUE_TYPE_STRING:
					return pushSimpleOpcodeValue(opcode, left_value, right_value);

				case OS_VALUE_TYPE_ARRAY:
				case OS_VALUE_TYPE_OBJECT:
				case OS_VALUE_TYPE_USERDATA:
				case OS_VALUE_TYPE_USERPTR:
					return pushObjectOpcodeValue(opcode, left_value, right_value, right_value.v.value, false);
				}
				break;

			case OS_VALUE_TYPE_ARRAY:
			case OS_VALUE_TYPE_OBJECT:
			case OS_VALUE_TYPE_USERDATA:
			case OS_VALUE_TYPE_USERPTR:
				switch(right_value.type){
				case OS_VALUE_TYPE_NULL:
				case OS_VALUE_TYPE_NUMBER:
				case OS_VALUE_TYPE_BOOL:
				case OS_VALUE_TYPE_STRING:
					// return pushObjectOpcodeValue(opcode, left_value, right_value, left_value.v.value, true);

				case OS_VALUE_TYPE_ARRAY:
				case OS_VALUE_TYPE_OBJECT:
				case OS_VALUE_TYPE_USERDATA:
				case OS_VALUE_TYPE_USERPTR:
					return pushObjectOpcodeValue(opcode, left_value, right_value, left_value.v.value, true);
				}
			}
			core->pushNull();
		}
	} lib = {this};

	StackValues * stack_values = &this->stack_values;
	if(stack_values->capacity < stack_values->count+1){
		reserveStackValues(stack_values->count+1);
	}
	switch(opcode){
	case Program::OP_COMPARE:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number - right_value.v.number;
			return;
		}
		stack_values->buf[stack_values->count++] = lib.compareValues(left_value, right_value);
		return;

	case Program::OP_LOGIC_PTR_EQ:
		stack_values->buf[stack_values->count++] = lib.isEqualExactly(left_value, right_value);
		return;

	case Program::OP_LOGIC_PTR_NE:
		stack_values->buf[stack_values->count++] = !lib.isEqualExactly(left_value, right_value);
		return;

	case Program::OP_LOGIC_EQ:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number == right_value.v.number;
			return;
		}
		if(left_value.type == OS_VALUE_TYPE_STRING && right_value.type == OS_VALUE_TYPE_STRING){
			stack_values->buf[stack_values->count++] = left_value.v.string == right_value.v.string;
			return;
		}
		stack_values->buf[stack_values->count++] = lib.compareValues(left_value, right_value) == 0;
		return;

	case Program::OP_LOGIC_NE:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number != right_value.v.number;
			return;
		}
		if(left_value.type == OS_VALUE_TYPE_STRING && right_value.type == OS_VALUE_TYPE_STRING){
			stack_values->buf[stack_values->count++] = left_value.v.string != right_value.v.string;
			return;
		}
		stack_values->buf[stack_values->count++] = lib.compareValues(left_value, right_value) != 0;
		return;

	case Program::OP_LOGIC_GE:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number >= right_value.v.number;
			return;
		}
		stack_values->buf[stack_values->count++] = lib.compareValues(left_value, right_value) >= 0;
		return;

	case Program::OP_LOGIC_LE:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number <= right_value.v.number;
			return;
		}
		stack_values->buf[stack_values->count++] = lib.compareValues(left_value, right_value) <= 0;
		return;

	case Program::OP_LOGIC_GREATER:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number > right_value.v.number;
			return;
		}
		stack_values->buf[stack_values->count++] = lib.compareValues(left_value, right_value) > 0;
		return;

	case Program::OP_LOGIC_LESS:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number < right_value.v.number;
			return;
		}
		stack_values->buf[stack_values->count++] = lib.compareValues(left_value, right_value) < 0;
		return;

	case Program::OP_CONCAT:
		if(left_value.type == OS_VALUE_TYPE_STRING && right_value.type == OS_VALUE_TYPE_STRING){
			pushStringValue(newStringValue(left_value.v.string, right_value.v.string));
			return;
		}
		break;

	case Program::OP_BIT_AND:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number & (OS_INT)right_value.v.number;
			return;
		}
		break;

	case Program::OP_BIT_OR:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number | (OS_INT)right_value.v.number;
			return;
		}
		break;

	case Program::OP_BIT_XOR:
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number ^ (OS_INT)right_value.v.number;
			return;
		}
		break;

	case Program::OP_ADD: // +
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number + right_value.v.number;
			return;
		}
		break;

	case Program::OP_SUB: // -
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number - right_value.v.number;
			return;
		}
		break;

	case Program::OP_MUL: // *
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = left_value.v.number * right_value.v.number;
			return;
		}
		break;

	case Program::OP_DIV: // /
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			if(!right_value.v.number){
				errorDivisionByZero();
				stack_values->buf[stack_values->count++] = 0.0;
				return;
			}
			stack_values->buf[stack_values->count++] = left_value.v.number / right_value.v.number;
			return;
		}
		break;

	case Program::OP_MOD: // %
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			if(!right_value.v.number){
				errorDivisionByZero();
				stack_values->buf[stack_values->count++] = 0.0;
				return;
			}
			stack_values->buf[stack_values->count++] = OS_MATH_MOD_OPERATOR(left_value.v.number, right_value.v.number);
			return;
		}
		break;

	case Program::OP_LSHIFT: // <<
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number << (OS_INT)right_value.v.number;
			return;
		}
		break;

	case Program::OP_RSHIFT: // >>
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number >> (OS_INT)right_value.v.number;
			return;
		}
		break;

	case Program::OP_POW: // **
		if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
			stack_values->buf[stack_values->count++] = OS_MATH_POW_OPERATOR((OS_FLOAT)left_value.v.number, (OS_FLOAT)right_value.v.number);
			return;
		}
		break;
	}
	return lib.pushBinaryOpcodeValue(opcode, left_value, right_value);
}

void OS::Core::setGlobalValue(const String& name, Value value, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	setPropertyValue(global_vars, Core::PropertyIndex(name), value, anonymous_setter_enabled, named_setter_enabled);
}

void OS::Core::setGlobalValue(const OS_CHAR * name, Value value, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	setGlobalValue(String(allocator, name), value, anonymous_setter_enabled, named_setter_enabled);
}

int OS::Core::getStackOffs(int offs)
{
	return offs <= 0 ? stack_values.count + offs + 1 : offs;
}

OS::Core::Value OS::Core::getStackValue(int offs)
{
	offs = offs <= 0 ? stack_values.count + offs : offs - 1;
	if(offs >= 0 && offs < stack_values.count){
		return stack_values.buf[offs];
	}
	if(offs == OS_REGISTER_GLOBALS - 1){
		return global_vars;
	}
	if(offs == OS_REGISTER_USERPOOL - 1){
		return user_pool;
	}
	// OS_ASSERT(false);
	return Value();
}

OS::Core::StackValues::StackValues()
{
	buf = NULL;
	capacity = 0;
	count = 0;
}

OS::Core::StackValues::~StackValues()
{
	OS_ASSERT(!buf && !capacity && !count);
}

void OS::Core::reserveStackValues(int new_capacity)
{
	if(stack_values.capacity < new_capacity){
		stack_values.capacity = (stack_values.capacity*2 + 16) & ~15;
		if(stack_values.capacity < new_capacity){
			stack_values.capacity = (new_capacity + 16) & ~15;
		}
		Value * new_buf = (Value*)malloc(sizeof(Value)*stack_values.capacity OS_DBG_FILEPOS);
		OS_MEMCPY(new_buf, stack_values.buf, sizeof(Value) * stack_values.count);
		free(stack_values.buf);
		stack_values.buf = new_buf;

		for(int i = 0; i < call_stack_funcs.count; i++){
			StackFunction * stack_func = &call_stack_funcs[i];
			OS_ASSERT(stack_func->locals_stack_pos >= 0);
			OS_ASSERT(stack_func->locals && stack_func->locals->is_stack_locals);
			stack_func->locals->locals = stack_values.buf + stack_func->locals_stack_pos;
		}

		if(stack_func){
			stack_func_locals = stack_func->locals->locals;
		}
	}
}

void OS::Core::removeStackValues(int offs, int count)
{
	if(count <= 0){
		OS_ASSERT(count == 0);
		return;
	}
	int start = offs <= 0 ? stack_values.count + offs : offs - 1;
	if(start < 0 || start >= stack_values.count){
		OS_ASSERT(false);
		return;
	}
	int end = start + count;
	if(end >= stack_values.count){
		OS_ASSERT(end == stack_values.count);
		stack_values.count = start;
	}else{
		count = stack_values.count - end;
		if(count == 1){
			stack_values.buf[start] = stack_values.buf[end];
		}else{
			OS_MEMMOVE(stack_values.buf + start, stack_values.buf + end, sizeof(Value) * count);
		}
		stack_values.count -= end - start;
	}
	// gcStepIfNeeded();
}

void OS::Core::removeStackValue(int offs)
{
	removeStackValues(offs, 1);
}

void OS::Core::removeAllStackValues()
{
	stack_values.count = 0;
	// gcStepIfNeeded();
}

void OS::Core::pop(int count)
{
	if(count >= stack_values.count){
		OS_ASSERT(count == stack_values.count);
		stack_values.count = 0;
	}else{
		stack_values.count -= count;
	}
	// gcStepIfNeeded();
}

void OS::Core::moveStackValues(int offs, int count, int new_offs)
{
	if(count <= 0){
		OS_ASSERT(count == 0);
		return;
	}
	offs = offs <= 0 ? stack_values.count + offs : offs - 1;
	if(offs < 0 || offs >= stack_values.count){
		OS_ASSERT(false);
		return;
	}
	int end = offs + count;
	if(end > stack_values.count){
		OS_ASSERT(false);
		return;
	}
	new_offs = new_offs <= 0 ? stack_values.count + new_offs : new_offs - 1;
	if(new_offs < 0 || new_offs >= stack_values.count){
		OS_ASSERT(false);
		return;
	}
	int new_end = new_offs + count;
	if(new_end > stack_values.count){
		OS_ASSERT(false);
		return;
	}
	Value * temp_values = (Value*)alloca(sizeof(Value) * count);
	OS_MEMCPY(temp_values, stack_values.buf + offs, sizeof(Value) * count);
	if(new_offs > offs){
		OS_MEMMOVE(stack_values.buf + offs, stack_values.buf + offs+count, sizeof(Value) * (new_offs - offs));
	}else{
		OS_MEMMOVE(stack_values.buf + new_offs+count, stack_values.buf + new_offs, sizeof(Value) * (offs - new_offs));
	}
	OS_MEMCPY(stack_values.buf + new_offs, temp_values, sizeof(Value) * count);
}

void OS::Core::moveStackValue(int offs, int new_offs)
{
	offs = offs <= 0 ? stack_values.count + offs : offs - 1;
	if(offs < 0 || offs >= stack_values.count){
		OS_ASSERT(false);
		return;
	}

	new_offs = new_offs <= 0 ? stack_values.count + new_offs : new_offs - 1;
	if(new_offs < 0 || new_offs >= stack_values.count){
		OS_ASSERT(false);
		return;
	}

	Value value = stack_values[offs];
	if(new_offs > offs){
		OS_MEMMOVE(stack_values.buf + offs, stack_values.buf + offs+1, sizeof(Value) * (new_offs - offs));
	}else{
		OS_MEMMOVE(stack_values.buf + new_offs+1, stack_values.buf + new_offs, sizeof(Value) * (offs - new_offs));
	}
	stack_values[new_offs] = value;
}

void OS::Core::insertValue(Value val, int offs)
{
	offs = offs <= 0 ? stack_values.count + offs : offs - 1;

	reserveStackValues(stack_values.count+1);
	stack_values.count++;

	if(offs < 0 || offs >= stack_values.count){
		OS_ASSERT(false);
		return;
	}
	int count = stack_values.count - offs - 1;
	if(count > 0){
		OS_MEMMOVE(stack_values.buf + offs+1, stack_values.buf + offs, sizeof(Value) * count);
	}
	stack_values[offs] = val;
}

void OS::pushNull()
{
	core->pushNull();
}

void OS::pushNumber(OS_INT32 val)
{
	core->pushNumber(val);
}

void OS::pushNumber(OS_INT64 val)
{
	core->pushNumber(val);
}

void OS::pushNumber(float val)
{
	core->pushNumber(val);
}

void OS::pushNumber(double val)
{
	core->pushNumber(val);
}

void OS::pushBool(bool val)
{
	core->pushBool(val);
}

void OS::pushString(const OS_CHAR * val)
{
	core->pushStringValue(val);
}

void OS::pushString(const OS_CHAR * val, int len)
{
	core->pushStringValue(val, len);
}

void OS::pushString(const Core::String& val)
{
	core->pushStringValue(val);
}

void OS::pushCFunction(OS_CFunction func, void * user_param)
{
	core->pushCFunctionValue(func, user_param);
}

void OS::pushCFunction(OS_CFunction func, int closure_values, void * user_param)
{
	core->pushCFunctionValue(func, closure_values, user_param);
}

void * OS::pushUserdata(int crc, int data_size, OS_UserdataDtor dtor, void * user_param)
{
	Core::GCUserdataValue * userdata = core->pushUserdataValue(crc, data_size, dtor, user_param);
	return userdata ? userdata->ptr : NULL;
}

void * OS::pushUserdata(int data_size, OS_UserdataDtor dtor, void * user_param)
{
	return pushUserdata(0, data_size, dtor, user_param);
}

void * OS::pushUserPointer(int crc, void * data, OS_UserdataDtor dtor, void * user_param)
{
	Core::GCUserdataValue * userdata = core->pushUserPointerValue(crc, data, dtor, user_param);
	return userdata ? userdata->ptr : NULL;
}

void * OS::pushUserPointer(void * data, OS_UserdataDtor dtor, void * user_param)
{
	return pushUserPointer(0, data, dtor, user_param);
}

void OS::newObject()
{
	core->pushObjectValue();
}

void OS::newArray(int initial_capacity)
{
	core->pushArrayValue(initial_capacity);
}

void OS::pushStackValue(int offs)
{
	core->pushStackValue(offs);
}

void OS::pushGlobals()
{
	pushStackValue(OS_REGISTER_GLOBALS);
}

void OS::pushUserPool()
{
	pushStackValue(OS_REGISTER_USERPOOL);
}

void OS::pushValueById(int id)
{
	core->pushValue(core->values.get(id));
}

void OS::retainValueById(int id)
{
	Core::GCValue * value = core->values.get(id);
	if(value){
		value->external_ref_count++;
	}
}

void OS::releaseValueById(int id)
{
	Core::GCValue * value = core->values.get(id);
	if(value){
		OS_ASSERT(value->external_ref_count > 0);
		if(!--value->external_ref_count && value->gc_color == Core::GC_WHITE){
			value->gc_color = Core::GC_BLACK;
		}
	}
}

void OS::clone(int offs)
{
	core->pushCloneValue(core->getStackValue(offs));
}

int OS::getStackSize()
{
	return core->stack_values.count;
}

int OS::getAbsoluteOffs(int offs)
{
	return core->getStackOffs(offs);
}

void OS::remove(int start_offs, int count)
{
	core->removeStackValues(start_offs, count);
}

void OS::pop(int count)
{
	core->pop(count);
}

void OS::removeAll()
{
	core->removeAllStackValues();
}

void OS::move(int start_offs, int count, int new_offs)
{
	core->moveStackValues(start_offs, count, new_offs);
}

void OS::move(int offs, int new_offs)
{
	core->moveStackValue(offs, new_offs);
}

bool OS::toBool(int offs)
{
	return core->valueToBool(core->getStackValue(offs));
}

bool OS::toBool(int offs, bool def)
{
	Core::Value value = core->getStackValue(offs);
	return value.isNull() ? def : core->valueToBool(value);
}

OS_NUMBER OS::toNumber(int offs, bool valueof_enabled)
{
	return core->valueToNumber(core->getStackValue(offs), valueof_enabled);
}

OS_NUMBER OS::toNumber(int offs, OS_NUMBER def, bool valueof_enabled)
{
	Core::Value value = core->getStackValue(offs);
	return value.isNull() ? def : core->valueToNumber(value, valueof_enabled);
}

float OS::toFloat(int offs, bool valueof_enabled)
{
	return (float)toNumber(offs, valueof_enabled);
}

float OS::toFloat(int offs, float def, bool valueof_enabled)
{
	return (float)toNumber(offs, (OS_NUMBER)def, valueof_enabled);
}

double OS::toDouble(int offs, bool valueof_enabled)
{
	return (double)toNumber(offs, valueof_enabled);
}

double OS::toDouble(int offs, double def, bool valueof_enabled)
{
	return (double)toNumber(offs, (OS_NUMBER)def, valueof_enabled);
}

int OS::toInt(int offs, bool valueof_enabled)
{
	return (int)toNumber(offs, valueof_enabled);
}

int OS::toInt(int offs, int def, bool valueof_enabled)
{
	return (int)toNumber(offs, (OS_NUMBER)def, valueof_enabled);
}

bool OS::isNumber(int offs, OS_NUMBER * out)
{
	return core->isValueNumber(core->getStackValue(offs), out);
}

OS::String OS::toString(int offs, bool valueof_enabled)
{
	return String(this, core->valueToString(core->getStackValue(offs), valueof_enabled));
}

OS::String OS::toString(int offs, const String& def, bool valueof_enabled)
{
	Core::Value value = core->getStackValue(offs);
	return value.isNull() ? def : String(this, core->valueToString(value, valueof_enabled));
}

bool OS::isString(int offs, String * out)
{
	return core->isValueString(core->getStackValue(offs), out);
}

bool OS::popBool()
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toBool(-1);
}

bool OS::popBool(bool def)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toBool(-1, def);
}

OS_NUMBER OS::popNumber(bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toNumber(-1, valueof_enabled);
}

OS_NUMBER OS::popNumber(OS_NUMBER def, bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toNumber(-1, def, valueof_enabled);
}

float OS::popFloat(bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toFloat(-1, valueof_enabled);
}

float OS::popFloat(float def, bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toFloat(-1, def, valueof_enabled);
}

double OS::popDouble(bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toDouble(-1, valueof_enabled);
}

double OS::popDouble(double def, bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toDouble(-1, def, valueof_enabled);
}

int OS::popInt(bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toInt(-1, valueof_enabled);
}

int OS::popInt(int def, bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toInt(-1, def, valueof_enabled);
}

OS::String OS::popString(bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toString(-1, valueof_enabled);
}

OS::String OS::popString(const String& def, bool valueof_enabled)
{
	struct Pop { OS * os; ~Pop(){ os->pop(); } } pop = {this};
	return toString(-1, def, valueof_enabled);
}

OS_EValueType OS::getType(int offs)
{
	return core->getStackValue(offs).type;
}

OS_EValueType OS::getTypeById(int id)
{
	Core::GCValue * val = core->values.get(id);
	return val ? val->type : OS_VALUE_TYPE_NULL;
}

bool OS::isType(OS_EValueType type, int offs)
{
	return core->getStackValue(offs).type == type;
}

bool OS::isNull(int offs)
{
	return isType(OS_VALUE_TYPE_NULL, offs);
}

bool OS::isObject(int offs)
{
	switch(core->getStackValue(offs).type){
	case OS_VALUE_TYPE_OBJECT:
		// case OS_VALUE_TYPE_ARRAY:
		return true;
	}
	return false;
}

bool OS::isUserdata(int crc, int offs, int prototype_crc)
{
	Core::Value val = core->getStackValue(offs);
	switch(val.type){
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		if(val.v.userdata->crc == crc){
			return true;
		}
		if(prototype_crc && val.v.userdata->prototype 
			&& core->isValuePrototypeOfUserdata(val.v.userdata->prototype, prototype_crc))
		{
			return true;
		}
	}
	return false;
}

void * OS::toUserdata(int crc, int offs, int prototype_crc)
{
	Core::Value val = core->getStackValue(offs);
	switch(val.type){
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		if(val.v.userdata->crc == crc){
			return val.v.userdata->ptr;
		}
		if(prototype_crc && val.v.userdata->prototype 
			&& core->isValuePrototypeOfUserdata(val.v.userdata->prototype, prototype_crc))
		{
			return val.v.userdata->ptr;
		}
	}
	return NULL;
}

void OS::clearUserdata(int crc, int offs, int prototype_crc)
{
	Core::Value val = core->getStackValue(offs);
	switch(val.type){
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		if(val.v.userdata->crc == crc){ // && val.v.userdata->ptr){
			core->clearValue(val.v.value);
			// val.v.userdata->ptr = NULL;
			return;
		}
		if(prototype_crc && val.v.userdata->prototype 
			&& core->isValuePrototypeOfUserdata(val.v.userdata->prototype, prototype_crc))
		{
			core->clearValue(val.v.value);
			return;
		}
	}
}

bool OS::isArray(int offs)
{
	return isType(OS_VALUE_TYPE_ARRAY, offs);
}

bool OS::isFunction(int offs)
{
	return core->getStackValue(offs).isFunction();
}

bool OS::Core::isValuePrototypeOf(GCValue * val, GCValue * prototype_val)
{
	while(val != prototype_val){
		val = val->prototype;
		if(!val){
			return false;
		}
	}
	return true;
}

bool OS::Core::isValuePrototypeOfUserdata(GCValue * val, int prototype_crc)
{
	for(int value_crc;;){
		switch(val->type){
		case OS_VALUE_TYPE_USERDATA:
		case OS_VALUE_TYPE_USERPTR:
			OS_ASSERT(dynamic_cast<GCUserdataValue*>(val));
			value_crc = ((GCUserdataValue*)val)->crc; // make crc visible in debugger
			if(value_crc == prototype_crc){
				return true;
			}
		}
		val = val->prototype;
		if(!val){
			return false;
		}
	}
	return false; // shut up compiler
}

bool OS::Core::isValueInstanceOf(GCValue * val, GCValue * prototype_val)
{
	return val->prototype ? isValuePrototypeOf(val->prototype, prototype_val) : false;
}

bool OS::Core::isValuePrototypeOf(Value val, Value prototype_val)
{
	GCValue * object = val.getGCValue();
	GCValue * proto = prototype_val.getGCValue();
	return object && proto && isValuePrototypeOf(object, proto);
}

bool OS::Core::isValueInstanceOf(Value val, Value prototype_val)
{
	GCValue * object = val.getGCValue();
	GCValue * proto = prototype_val.getGCValue();
	return object && proto && isValueInstanceOf(object, proto);
}

bool OS::isPrototypeOf(int value_offs, int prototype_offs)
{
	return core->isValuePrototypeOf(core->getStackValue(value_offs), core->getStackValue(prototype_offs));
}

bool OS::is(int value_offs, int prototype_offs)
{
	return core->isValueInstanceOf(core->getStackValue(value_offs), core->getStackValue(prototype_offs));
}

void OS::setProperty(bool anonymous_setter_enabled, bool named_setter_enabled)
{
	if(core->stack_values.count >= 3){
		Core::Value object = core->stack_values[core->stack_values.count - 3];
		Core::Value index = core->stack_values[core->stack_values.count - 2];
		Core::Value value = core->stack_values[core->stack_values.count - 1];
		core->setPropertyValue(object, Core::PropertyIndex(index), value, anonymous_setter_enabled, named_setter_enabled);
		pop(3);
	}else{
		// error
		pop(3);
	}
}

void OS::setProperty(const OS_CHAR * name, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	setProperty(Core::String(this, name), anonymous_setter_enabled, named_setter_enabled);
}

void OS::setProperty(const Core::String& name, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	if(core->stack_values.count >= 2){
		Core::Value object = core->stack_values[core->stack_values.count - 2];
		Core::Value value = core->stack_values[core->stack_values.count - 1];
		core->setPropertyValue(object, Core::PropertyIndex(name), value, anonymous_setter_enabled, named_setter_enabled);
		pop(2);
	}else{
		// error
		pop(2);
	}
}

void OS::setProperty(int offs, const OS_CHAR * name, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	setProperty(offs, Core::String(this, name), anonymous_setter_enabled, named_setter_enabled);
}

void OS::setProperty(int offs, const Core::String& name, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	if(core->stack_values.count >= 1){
		Core::Value object = core->getStackValue(offs);
		Core::Value value = core->stack_values[core->stack_values.count - 1];
		core->setPropertyValue(object, Core::PropertyIndex(name), value, anonymous_setter_enabled, named_setter_enabled);
		pop();
	}else{
		// error
		pop();
	}
}

void OS::addProperty()
{
	Core::Value value = core->getStackValue(-2);
	switch(value.type){
	case OS_VALUE_TYPE_ARRAY:
		core->insertValue(value.v.arr->values.count, -1);
		break;

	case OS_VALUE_TYPE_OBJECT:
		core->insertValue(value.v.object->table ? value.v.object->table->next_index : 0, -1);
		break;
	}
	setProperty(false, false);
}

void OS::deleteProperty(bool anonymous_del_enabled, bool named_del_enabled)
{
	core->deleteValueProperty(core->getStackValue(-2), core->getStackValue(-1), anonymous_del_enabled, named_del_enabled, false);
	pop(2);
}

void OS::deleteProperty(const OS_CHAR * name, bool anonymous_del_enabled, bool named_del_enabled)
{
	deleteProperty(Core::String(this, name), anonymous_del_enabled, named_del_enabled);
}

void OS::deleteProperty(const Core::String& name, bool anonymous_del_enabled, bool named_del_enabled)
{
	pushString(name);
	deleteProperty(anonymous_del_enabled, named_del_enabled);
}

void OS::getPrototype()
{
	if(core->stack_values.count >= 1){
		core->pushPrototype(core->stack_values.lastElement());
	}else{
		pushNull();
	}
}

void OS::setPrototype()
{
	setPrototype(0);
}

void OS::setPrototype(int userdata_crc)
{
	if(core->stack_values.count >= 2){
		Core::Value value = core->stack_values[core->stack_values.count - 2];
		Core::Value proto = core->stack_values[core->stack_values.count - 1];
		core->setPrototype(value, proto, userdata_crc);
	}
	pop(2);
}

int OS::getValueId(int offs)
{
	Core::Value val = core->getStackValue(offs);
	switch(val.type){
	case OS_VALUE_TYPE_STRING:
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		return val.v.value->value_id;

	case OS_VALUE_TYPE_WEAKREF:
		return val.v.value_id;
	}
	return 0;
}

bool OS::Core::getPropertyValue(Value& result, Table * table, const PropertyIndex& index)
{
#if defined OS_DEBUG && defined OS_WARN_NULL_INDEX
	if(table != check_recursion->table && index.index.type == OS_VALUE_TYPE_NULL){
		error(OS_E_WARNING, OS_TEXT("object get null index"));
	}
#endif
	if(table){
		Property * prop = table->get(index);
		if(prop){
			result = prop->value;
			return true;
		}
	}
	return false;
}

bool OS::Core::getPropertyValue(Value& result, GCValue * table_value, const PropertyIndex& index, bool prototype_enabled)
{
#if defined OS_DEBUG && defined OS_WARN_NULL_INDEX
	if(table_value != check_recursion && index.index.type == OS_VALUE_TYPE_NULL){
		error(OS_E_WARNING, OS_TEXT("object get null index"));
	}
#endif

	if(table_value->type == OS_VALUE_TYPE_ARRAY && index.index.type == OS_VALUE_TYPE_NUMBER){
		OS_ASSERT(dynamic_cast<GCArrayValue*>(table_value));
		int i = (int)index.index.v.number;
		if((i >= 0 || (i += ((GCArrayValue*)table_value)->values.count) >= 0) && i < ((GCArrayValue*)table_value)->values.count){
			result = ((GCArrayValue*)table_value)->values[i];
		}else{
			result = Value();
		}
		return true;
	}
	Property * prop = NULL;
	Table * table = table_value->table;
	if(table && (prop = table->get(index))){
		result = prop->value;
		return true;
	}
	if(prototype_enabled){
		GCValue * cur_value = table_value;
		while(cur_value->prototype){
			cur_value = cur_value->prototype;
			Table * cur_table = cur_value->table;
			if(cur_table && (prop = cur_table->get(index))){
				result = prop->value;
				return true;
			}
		}
	}
	if(index.index.type == OS_VALUE_TYPE_STRING && strings->syntax_prototype == index.index.v.string){
		result = table_value->prototype;
		return true;
	}
	if(table_value->type == OS_VALUE_TYPE_ARRAY){
		OS_ASSERT(dynamic_cast<GCArrayValue*>(table_value));
		OS_NUMBER number;
		if(isValueNumber(index.index, &number)){
			int i = (int)number;
			if((i >= 0 || (i += ((GCArrayValue*)table_value)->values.count) >= 0) && i < ((GCArrayValue*)table_value)->values.count){
				result = ((GCArrayValue*)table_value)->values[i];
			}else{
				result = Value();
			}
			return true;
		}
	}
	return false;
}

bool OS::Core::getPropertyValue(Value& result, Value table_value, const PropertyIndex& index, bool prototype_enabled)
{
	switch(table_value.type){
	case OS_VALUE_TYPE_NULL:
		return false;

	case OS_VALUE_TYPE_BOOL:
		return prototype_enabled && getPropertyValue(result, prototypes[PROTOTYPE_BOOL], index, prototype_enabled);

	case OS_VALUE_TYPE_NUMBER:
		return prototype_enabled && getPropertyValue(result, prototypes[PROTOTYPE_NUMBER], index, prototype_enabled);

	case OS_VALUE_TYPE_STRING:
		// return prototype_enabled && getPropertyValue(result, prototypes[PROTOTYPE_STRING], index, prototype_enabled);

	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		return getPropertyValue(result, table_value.v.value, index, prototype_enabled);
	}
	return false;
}

bool OS::Core::hasProperty(GCValue * table_value, const PropertyIndex& index, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	Value value;
	if(getPropertyValue(value, table_value, index, prototype_enabled)){
		return value.type != OS_VALUE_TYPE_NULL;
	}
	if(!anonymous_getter_enabled && !named_getter_enabled){
		return false;
	}
	if(hasSpecialPrefix(index.index)){
		return false;
	}
	if(index.index.type == OS_VALUE_TYPE_STRING && named_getter_enabled){
		const void * buf1 = strings->__getAt.toChar();
		int size1 = strings->__getAt.getDataSize();
		const void * buf2 = index.index.v.string->toChar();
		int size2 = index.index.v.string->getDataSize();
		GCStringValue * getter_name = newStringValue(buf1, size1, buf2, size2);
		if(getPropertyValue(value, table_value, PropertyIndex(getter_name, PropertyIndex::KeepStringIndex()), prototype_enabled)){
			return true;
		}
	}
	if(anonymous_getter_enabled){
		// TODO: add __isset method ???
	}
	return false;
}

void OS::Core::pushPropertyValue(GCValue * table_value, const PropertyIndex& index, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled, bool auto_create)
{
	GCValue * self = table_value;
	for(;;){
		Value value;
		if(getPropertyValue(value, table_value, index, prototype_enabled)){
			return pushValue(value);
		}
		if((anonymous_getter_enabled || named_getter_enabled) && !hasSpecialPrefix(index.index)){
			if(index.index.type == OS_VALUE_TYPE_STRING && named_getter_enabled){
				const void * buf1 = strings->__getAt.toChar();
				int size1 = strings->__getAt.getDataSize();
				const void * buf2 = index.index.v.string->toChar();
				int size2 = index.index.v.string->getDataSize();
				GCStringValue * getter_name = newStringValue(buf1, size1, buf2, size2);
				if(getPropertyValue(value, table_value, PropertyIndex(getter_name, PropertyIndex::KeepStringIndex()), prototype_enabled)){
					pushValue(value);
					pushValue(self);
					call(0, 1);
					return;
				}
			}
			if(anonymous_getter_enabled && getPropertyValue(value, table_value, PropertyIndex(strings->__get, PropertyIndex::KeepStringIndex()), prototype_enabled)){
				// auto_create = false;
				if(value.type == OS_VALUE_TYPE_OBJECT){
					table_value = value.v.value;
					continue;
				}
				pushValue(value);
				pushValue(self);
				pushValue(index.index);
				if(!auto_create){
					call(1, 1);
				}else{
					pushBool(true);
					call(2, 1);
				}
				if(auto_create && stack_values.lastElement().type == OS_VALUE_TYPE_NULL){
					pop();
					setPropertyValue(self, index, Value(pushObjectValue()), false, false); 
				}
				return;
			}
		}
		if(auto_create){
			setPropertyValue(self, index, Value(pushObjectValue()), false, false); 
			return;
		}
		break;
	}
	return pushNull();
}

void OS::Core::pushPropertyValueForPrimitive(Value self, const PropertyIndex& index, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled, bool auto_create)
{
	GCValue * proto;
	switch(self.type){
	case OS_VALUE_TYPE_NUMBER:
		proto = prototypes[PROTOTYPE_NUMBER];
		break;

	case OS_VALUE_TYPE_BOOL:
		proto = prototypes[PROTOTYPE_BOOL];
		break;

	default:
		pushNull();
		return;
	}
	// GCValue * self = table_value;
	for(;;){
		OS_ASSERT(proto);
		Value value;
		if(prototype_enabled && getPropertyValue(value, proto, index, prototype_enabled)){
			return pushValue(value);
		}
		if((anonymous_getter_enabled || named_getter_enabled) && !hasSpecialPrefix(index.index)){
			if(index.index.type == OS_VALUE_TYPE_STRING && named_getter_enabled){
				const void * buf1 = strings->__getAt.toChar();
				int size1 = strings->__getAt.getDataSize();
				const void * buf2 = index.index.v.string->toChar();
				int size2 = index.index.v.string->getDataSize();
				GCStringValue * getter_name = newStringValue(buf1, size1, buf2, size2);
				if(getPropertyValue(value, proto, PropertyIndex(getter_name, PropertyIndex::KeepStringIndex()), prototype_enabled)){
					pushValue(value);
					pushValue(self);
					call(0, 1);
					return;
				}
			}
			if(anonymous_getter_enabled && getPropertyValue(value, proto, PropertyIndex(strings->__get, PropertyIndex::KeepStringIndex()), prototype_enabled)){
				// auto_create = false;
				if(value.type == OS_VALUE_TYPE_OBJECT){
					proto = value.v.value;
					continue;
				}
				pushValue(value);
				pushValue(self);
				pushValue(index.index);
				if(!auto_create){
					call(1, 1);
				}else{
					pushBool(true);
					call(2, 1);
				}
				if(auto_create && stack_values.lastElement().type == OS_VALUE_TYPE_NULL){
					pop();
					setPropertyValue(self, index, Value(pushObjectValue()), false, false); 
				}
				return;
			}
		}
		if(auto_create){
			setPropertyValue(self, index, Value(pushObjectValue()), false, false); 
			return;
		}
		break;
	}
	return pushNull();
}

void OS::Core::pushPropertyValue(Value table_value, const PropertyIndex& index, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled, bool auto_create)
{
	switch(table_value.type){
	case OS_VALUE_TYPE_NULL:
		break;

	case OS_VALUE_TYPE_BOOL:
		/* if(prototype_enabled){
			return pushPropertyValue(prototypes[PROTOTYPE_BOOL], index, anonymous_getter_enabled, named_getter_enabled, prototype_enabled, auto_create);
		}
		break; */

	case OS_VALUE_TYPE_NUMBER:
		/* if(prototype_enabled){
			return pushPropertyValue(prototypes[PROTOTYPE_NUMBER], index, anonymous_getter_enabled, named_getter_enabled, prototype_enabled, auto_create);
		}
		break; */
		return pushPropertyValueForPrimitive(table_value, index, anonymous_getter_enabled, named_getter_enabled, prototype_enabled, auto_create);

	case OS_VALUE_TYPE_STRING:
		/* if(prototype_enabled){
			return pushPropertyValue(prototypes[PROTOTYPE_STRING], index, anonymous_getter_enabled, named_getter_enabled, prototype_enabled, auto_create);
		}
		break; */

	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		return pushPropertyValue(table_value.v.value, index, anonymous_getter_enabled, named_getter_enabled, prototype_enabled, auto_create);
	}
	pushNull();
}

void OS::getProperty(bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	if(core->stack_values.count >= 2){
		Core::Value object = core->stack_values[core->stack_values.count - 2];
		Core::Value index = core->stack_values[core->stack_values.count - 1];
		// core->stack_values.count -= 2;
		core->pushPropertyValue(object, Core::PropertyIndex(index), anonymous_getter_enabled, named_getter_enabled, prototype_enabled, false);
		core->removeStackValues(-3, 2);
	}else{
		// error
		pop(2);
		pushNull();
	}
}

void OS::getProperty(const OS_CHAR * name, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	getProperty(Core::String(this, name), anonymous_getter_enabled, named_getter_enabled, prototype_enabled);
}

void OS::getProperty(const Core::String& name, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	pushString(name);
	getProperty(anonymous_getter_enabled, named_getter_enabled, prototype_enabled);
}

void OS::getProperty(int offs, const OS_CHAR * name, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	getProperty(offs, Core::String(this, name), anonymous_getter_enabled, named_getter_enabled, prototype_enabled);
}

void OS::getProperty(int offs, const Core::String& name, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	pushStackValue(offs);
	getProperty(name, anonymous_getter_enabled, named_getter_enabled, prototype_enabled);
}

void OS::Core::releaseUpvalues(Upvalues * upvalues)
{
	if(--upvalues->ref_count > 0){
		return;
	}
	deleteUpvalues(upvalues);
}

void OS::Core::deleteUpvalues(Upvalues * upvalues)
{
	upvalues->prog->release();
	if(upvalues->num_parents > 0){
		releaseUpvalues(upvalues->getParent(0));
	}
	if(!upvalues->is_stack_locals){
		free(upvalues->locals);
	}
	free(upvalues);
}

void OS::Core::clearStackFunction(StackFunction * stack_func)
{
	// OS_ASSERT(stack_func->func && stack_func->func->type == OS_VALUE_TYPE_FUNCTION);
	// OS_ASSERT(stack_func->func->value.func->func_decl);
	// OS_ASSERT(stack_func->self);
	// OS_ASSERT(stack_func->func->value.func->parent_inctance != stack_func);

	if(--stack_func->locals->ref_count > 0){
		int count = stack_func->locals->num_locals; // >opcode_stack_pos - stack_func->locals_stack_pos;
		if(count > 0){
			Value * locals = (Value*)malloc(sizeof(Value) * count OS_DBG_FILEPOS);
			OS_MEMCPY(locals, stack_func->locals->locals, sizeof(Value) * count);
			stack_func->locals->locals = locals;
		}else{
			stack_func->locals->locals = NULL;
		}
		stack_func->locals->is_stack_locals = false;
	}else{
		deleteUpvalues(stack_func->locals);
	}

	stack_func->func = NULL;
	stack_func->self = NULL;
	stack_func->self_for_proto = NULL;
	stack_func->locals = NULL;
	stack_func->arguments = NULL;
	stack_func->rest_arguments = NULL;

	stack_func->~StackFunction();
	// free(stack_func);
}

void OS::Core::enterFunction(GCFunctionValue * func_value, Value self, GCValue * self_for_proto, int params, int extra_remove_from_stack, int need_ret_values)
{
	OS_ASSERT(call_stack_funcs.count < OS_CALL_STACK_MAX_SIZE);
	OS_ASSERT(func_value->type == OS_VALUE_TYPE_FUNCTION);
	OS_ASSERT(stack_values.count >= params + extra_remove_from_stack);
	// OS_ASSERT(self);

	FunctionDecl * func_decl = func_value->func_decl;
	int num_extra_params = params > func_decl->num_params ? params - func_decl->num_params : 0;
	// int locals_mem_size = sizeof(Value) * (func_decl->num_locals + num_extra_params);
	// int parents_mem_size = sizeof(FunctionRunningInstance*) * func_decl->max_up_count;

	// stack has to be reserved here!!! don't move it
	reserveStackValues(stack_values.count - params + func_decl->num_locals + num_extra_params);

	// allocator->vectorReserveCapacity(call_stack_funcs, call_stack_funcs.count+1 OS_DBG_FILEPOS);
	if(call_stack_funcs.capacity < call_stack_funcs.count+1){
		call_stack_funcs.capacity = call_stack_funcs.capacity > 0 ? call_stack_funcs.capacity*2 : 8;
		OS_ASSERT(call_stack_funcs.capacity >= call_stack_funcs.count+1);

		StackFunction * new_buf = (StackFunction*)malloc(sizeof(StackFunction)*call_stack_funcs.capacity OS_DBG_FILEPOS);
		OS_MEMCPY(new_buf, call_stack_funcs.buf, sizeof(StackFunction) * call_stack_funcs.count);
		free(call_stack_funcs.buf);
		call_stack_funcs.buf = new_buf;
	}

	// StackFunction * stack_func = new (malloc(sizeof(StackFunction) OS_DBG_FILEPOS)) StackFunction();
	// StackFunction * stack_func = new (call_stack_funcs.buf + call_stack_funcs.count++) StackFunction();
	StackFunction * stack_func = (StackFunction*)(call_stack_funcs.buf + call_stack_funcs.count++);
	stack_func->func = func_value;
	stack_func->self = self;
	stack_func->self_for_proto = self_for_proto ? self_for_proto : self.getGCValue();
	if(!stack_func->self_for_proto){
		switch(self.type){
		case OS_VALUE_TYPE_BOOL:
			stack_func->self_for_proto = prototypes[PROTOTYPE_BOOL];
			break;

		case OS_VALUE_TYPE_NUMBER:
			stack_func->self_for_proto = prototypes[PROTOTYPE_NUMBER];
			break;
		}
	}

	stack_func->caller_stack_pos = stack_values.count - params - extra_remove_from_stack;
	stack_func->locals_stack_pos = stack_func->caller_stack_pos + extra_remove_from_stack;
	stack_func->stack_pos = stack_func->locals_stack_pos + func_decl->num_locals + num_extra_params;

	// reserveStackValues(stack_func->bottom_stack_pos);
	OS_ASSERT(stack_values.capacity >= stack_func->stack_pos);
	stack_values.count = stack_func->stack_pos;

	stack_func->num_params = params;
	stack_func->num_extra_params = num_extra_params;

	Upvalues * func_locals = (Upvalues*)(malloc(sizeof(Upvalues) + sizeof(Upvalues*) * func_decl->func_depth OS_DBG_FILEPOS));
	func_locals->prog = func_value->prog->retain();
	func_locals->func_decl = func_decl;
	func_locals->locals = stack_values.buf + stack_func->locals_stack_pos;
	func_locals->num_locals = func_decl->num_locals;
	func_locals->is_stack_locals = true;
	func_locals->num_parents = func_decl->func_depth;
	func_locals->ref_count = 1;
	func_locals->gc_time = -1;
	if(func_decl->func_depth > 0){
		OS_ASSERT(func_value->upvalues && func_value->upvalues->num_parents == func_decl->func_depth-1);
		Upvalues ** parents = func_locals->getParents();
		parents[0] = func_value->upvalues->retain();
		if(func_decl->func_depth > 1){
			OS_MEMCPY(parents+1, func_value->upvalues->getParents(), sizeof(Upvalues*) * (func_decl->func_depth-1));
		}
	}
	stack_func->locals = func_locals;

	Value * extra_params = func_locals->locals + func_decl->num_locals;
	if(num_extra_params > 0 && func_decl->num_locals != params - num_extra_params){
		OS_MEMCPY(extra_params, func_locals->locals + (params - num_extra_params), sizeof(Value) * num_extra_params);
	}
	int func_params = func_decl->num_params < params ? func_decl->num_params : params;
	OS_ASSERT(func_params <= func_decl->num_locals);
	if(func_decl->num_locals > func_params){
		OS_MEMSET(func_locals->locals + func_params, 0, sizeof(Value) * (func_decl->num_locals - func_params));
	}

	stack_func->need_ret_values = need_ret_values;
	// stack_func->opcode_offs = 0; // func_decl->opcodes_pos;

	new (&stack_func->opcodes) MemStreamReader(NULL, func_value->prog->opcodes->buffer + func_decl->opcodes_pos, func_decl->opcodes_size);

	func_locals->locals[func_decl->num_params + VAR_ENV] = func_value->env;
#ifdef OS_GLOBAL_VAR_ENABLED
	func_locals->locals[func_decl->num_params + VAR_GLOBALS] = global_vars;
#endif

	reloadStackFunctionCache();

	// gcMarkStackFunction(stack_func);
}

int OS::Core::opBreakFunction()
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= stack_func->stack_pos);
	int cur_ret_values = 0;
	int ret_values = stack_func->need_ret_values;
	stack_values.count = stack_func->stack_pos;
	ret_values = syncRetValues(ret_values, cur_ret_values);
	OS_ASSERT(stack_values.count == stack_func->stack_pos + ret_values);
	// stack_func->opcodes_pos = opcodes.getPos();
	OS_ASSERT(call_stack_funcs.count > 0 && &call_stack_funcs[call_stack_funcs.count-1] == stack_func);
	call_stack_funcs.count--;
	clearStackFunction(stack_func);
	removeStackValues(stack_func->caller_stack_pos+1, stack_values.count - ret_values - stack_func->caller_stack_pos);
	reloadStackFunctionCache();
	return ret_values;
}

void OS::Core::opDebugger()
{
	StackFunction * stack_func = this->stack_func;
	int line = stack_func->opcodes.readUVariable();
	int pos = stack_func->opcodes.readUVariable();
	int saved_lines = stack_func->opcodes.readUVariable();
	const OS_CHAR * lines[OS_DEBUGGER_SAVE_NUM_LINES];
	OS_MEMSET(lines, 0, sizeof(lines));
	GCStringValue ** prog_strings = stack_func_prog_strings; // stack_func->func->prog->const_strings;
	int prog_num_strings = stack_func->func->prog->num_strings;
	for(int i = 0; i < saved_lines; i++){
		int offs = stack_func->opcodes.readUVariable();
		OS_ASSERT(offs >= 0 && offs < prog_num_strings);
		OS_ASSERT(prog_strings[offs]->type == OS_VALUE_TYPE_STRING);
		if(i < OS_DEBUGGER_SAVE_NUM_LINES){
			lines[i] = prog_strings[offs]->toChar();
		}
	}
	DEBUG_BREAK;
}

/*
void OS::Core::opPushNumber()
{
	// StackFunction * stack_func = this->stack_func;
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_numbers);
	pushNumber(stack_func_prog_numbers[i]);
}

void OS::Core::opPushString()
{
	StackFunction * stack_func = this->stack_func;
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_strings);
	OS_ASSERT(stack_func_prog_strings[i]->type == OS_VALUE_TYPE_STRING);
	pushValue(stack_func_prog_strings[i]);
}
*/

void OS::Core::opPushFunction()
{
	StackFunction * stack_func = this->stack_func;
	int prog_func_index = stack_func->opcodes.readUVariable();
	Program * prog = stack_func->func->prog;
	OS_ASSERT(prog_func_index > 0 && prog_func_index < prog->num_functions);
	FunctionDecl * func_decl = prog->functions + prog_func_index;
	// int env_index = stack_func->func->func_decl->num_params + VAR_ENV;
	pushValue(newFunctionValue(stack_func, prog, func_decl, stack_func_locals[stack_func_env_index]));
	stack_func->opcodes.movePos(func_decl->opcodes_size);
}

void OS::Core::opPushArray()
{
	pushArrayValue(stack_func->opcodes.readByte());
}

void OS::Core::opPushObject()
{
	pushObjectValue();
}

void OS::Core::opObjectSetByAutoIndex()
{
	OS_ASSERT(stack_values.count >= 2);
	Value object = stack_values[stack_values.count-2];
	OS_INT num_index = 0;
	switch(object.type){
	case OS_VALUE_TYPE_ARRAY:
		OS_ASSERT(dynamic_cast<GCArrayValue*>(object.v.arr));
		num_index = object.v.arr->values.count;
		break;

	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		num_index = object.v.object->table ? object.v.object->table->next_index : 0;
		break;
	}
	setPropertyValue(object, PropertyIndex(num_index), stack_values.lastElement(), false, false);
	pop(); // keep only object in stack
}

void OS::Core::opObjectSetByExp()
{
	OS_ASSERT(stack_values.count >= 3);
	setPropertyValue(stack_values[stack_values.count - 3], 
		Core::PropertyIndex(stack_values[stack_values.count - 2]), 
		stack_values[stack_values.count - 1], false, false);
	pop(2); // keep only object in stack
}

void OS::Core::opObjectSetByIndex()
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 2);
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_numbers);
	setPropertyValue(stack_values[stack_values.count-2], 
		PropertyIndex(stack_func->func->prog->const_numbers[i]), 
		stack_values.lastElement(), false, false);
	pop(); // keep only object in stack
}

void OS::Core::opObjectSetByName()
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 2);
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_strings);
	GCStringValue * name = stack_func_prog_strings[i];
	OS_ASSERT(name->type == OS_VALUE_TYPE_STRING);
	setPropertyValue(stack_values[stack_values.count-2], 
		PropertyIndex(name, PropertyIndex::KeepStringIndex()), 
		stack_values.lastElement(), false, false);
	pop(); // keep only object in stack
}

void OS::Core::opPushEnvVar(bool auto_create)
{
	StackFunction * stack_func = this->stack_func;
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_strings);
	GCStringValue * name = stack_func_prog_strings[i];
	OS_ASSERT(name->type == OS_VALUE_TYPE_STRING);
	// int env_index = stack_func->func->func_decl->num_params + VAR_ENV;
	pushPropertyValue(stack_func_locals[stack_func_env_index], 
		PropertyIndex(name, PropertyIndex::KeepStringIndex()), 
		true, true, true, auto_create); 
}

void OS::Core::opSetEnvVar()
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 1);
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_strings);
	GCStringValue * name = stack_func_prog_strings[i];
	OS_ASSERT(name->type == OS_VALUE_TYPE_STRING);
	// int env_index = stack_func->func->func_decl->num_params + VAR_ENV;
	setPropertyValue(stack_func_locals[stack_func_env_index], 
		PropertyIndex(name, PropertyIndex::KeepStringIndex()), 
		stack_values.lastElement(), true, true);
	pop();
}

void OS::Core::opPushThis()
{
	StackFunction * stack_func = this->stack_func;
	pushValue(stack_func->self);
}

void OS::Core::opPushArguments()
{
	StackFunction * stack_func = this->stack_func;
	pushArguments(stack_func);
}

void OS::Core::opPushRestArguments()
{
	StackFunction * stack_func = this->stack_func;
	pushRestArguments(stack_func);
}

void OS::Core::opPushLocalVar()
{
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(i < num_stack_func_locals);
	pushValue(stack_func_locals[i]);
}

void OS::Core::opPushLocalVarAutoCreate()
{
	// StackFunction * stack_func = this->stack_func;
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(i < num_stack_func_locals);
	if(stack_func_locals[i].type == OS_VALUE_TYPE_NULL){
		stack_func_locals[i] = newObjectValue();
	}
	pushValue(stack_func_locals[i]);
}

void OS::Core::opSetLocalVar()
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 1);
	int i = stack_func->opcodes.readUVariable();
	// Upvalues * func_upvalues = stack_func->locals;
	OS_ASSERT(i < num_stack_func_locals);
	StackValues * stack_values = &this->stack_values;
	switch((stack_func_locals[i] = stack_values->buf[--stack_values->count]).type){
	case OS_VALUE_TYPE_FUNCTION:
		OS_ASSERT(dynamic_cast<GCFunctionValue*>(stack_func_locals[i].v.func));
		if(!stack_func_locals[i].v.func->name){
			stack_func_locals[i].v.func->name = stack_func->func->func_decl->locals[i].name.string;
		}
		break;

	case OS_VALUE_TYPE_CFUNCTION:
		OS_ASSERT(dynamic_cast<GCCFunctionValue*>(stack_func_locals[i].v.cfunc));
		if(!stack_func_locals[i].v.cfunc->name){
			stack_func_locals[i].v.cfunc->name = stack_func->func->func_decl->locals[i].name.string;
		}
		break;
	}
	// already removed
	// pop();
}

void OS::Core::opPushUpvalue()
{
	StackFunction * stack_func = this->stack_func;
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
	int up_count = *stack_func->opcodes.cur++; // readByte();
	OS_ASSERT(up_count <= stack_func->func->func_decl->max_up_count);
	Upvalues * func_upvalues = stack_func->locals;
	OS_ASSERT(up_count <= func_upvalues->num_parents);
	Upvalues * scope = func_upvalues->getParent(up_count-1);
	OS_ASSERT(i < scope->num_locals);
	pushValue(scope->locals[i]);
}

void OS::Core::opPushUpvalueAutoCreate()
{
	StackFunction * stack_func = this->stack_func;
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
	int up_count = *stack_func->opcodes.cur++; // readByte();
	OS_ASSERT(up_count <= stack_func->func->func_decl->max_up_count);
	Upvalues * func_upvalues = stack_func->locals;
	OS_ASSERT(up_count <= func_upvalues->num_parents);
	Upvalues * scope = func_upvalues->getParent(up_count-1);
	OS_ASSERT(i < scope->num_locals);
	if(scope->locals[i].type == OS_VALUE_TYPE_NULL){
		scope->locals[i] = newObjectValue();
	}
	pushValue(scope->locals[i]);
}

void OS::Core::opSetUpvalue()
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 1);
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
	int up_count = *stack_func->opcodes.cur++; // readByte();
	OS_ASSERT(up_count <= stack_func->func->func_decl->max_up_count);
	Upvalues * func_upvalues = stack_func->locals;
	OS_ASSERT(up_count <= func_upvalues->num_parents);
	Upvalues * scope = func_upvalues->getParent(up_count-1);
	OS_ASSERT(i < scope->num_locals);
	switch((scope->locals[i] = stack_values.lastElement()).type){
	case OS_VALUE_TYPE_FUNCTION:
		OS_ASSERT(dynamic_cast<GCFunctionValue*>(scope->locals[i].v.func));
		if(!scope->locals[i].v.func->name){
			scope->locals[i].v.func->name = scope->func_decl->locals[i].name.string;
		}
		break;

	case OS_VALUE_TYPE_CFUNCTION:
		OS_ASSERT(dynamic_cast<GCCFunctionValue*>(scope->locals[i].v.cfunc));
		if(!scope->locals[i].v.cfunc->name){
			scope->locals[i].v.cfunc->name = scope->func_decl->locals[i].name.string;
		}
		break;
	}
	// pop();
	--stack_values.count;
}

void OS::Core::opIfJump1(bool boolean)
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 1);
	// Value value = stack_values.lastElement();
	if(valueToBool(stack_values.lastElement()) == boolean){
		// int offs = stack_func->opcodes.readInt16();
		OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
		int offs = (OS_INT8)stack_func->opcodes.cur[0];
		// stack_func->opcodes.movePos(offs);
		OS_ASSERT(stack_func->opcodes.getPos()+offs >= 0 && stack_func->opcodes.getPos()+offs <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += offs;
	}else{
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) >= 0);
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += sizeof(OS_INT32);
	}
	// pop();
	--stack_values.count;
}

void OS::Core::opIfJump2(bool boolean)
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 1);
	// Value value = stack_values.lastElement();
	if(valueToBool(stack_values.lastElement()) == boolean){
		// int offs = stack_func->opcodes.readInt16();
		OS_ASSERT(stack_func->opcodes.getPos()+2 <= stack_func->opcodes.getSize());
		int offs = (OS_INT16)(stack_func->opcodes.cur[0] | (stack_func->opcodes.cur[1] << 8));
		// stack_func->opcodes.movePos(offs);
		OS_ASSERT(stack_func->opcodes.getPos()+offs >= 0 && stack_func->opcodes.getPos()+offs <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += offs;
	}else{
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) >= 0);
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += sizeof(OS_INT32);
	}
	// pop();
	--stack_values.count;
}

void OS::Core::opIfJump4(bool boolean)
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 1);
	// Value value = stack_values.lastElement();
	if(valueToBool(stack_values.lastElement()) == boolean){
		int offs = stack_func->opcodes.readInt32();
		// stack_func->opcodes.movePos(offs);
		OS_ASSERT(stack_func->opcodes.getPos()+offs >= 0 && stack_func->opcodes.getPos()+offs <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += offs;
	}else{
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) >= 0);
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += sizeof(OS_INT32);
	}
	// pop();
	--stack_values.count;
}

void OS::Core::opJump4()
{
	StackFunction * stack_func = this->stack_func;
	int offs = stack_func->opcodes.readInt32();
	stack_func->opcodes.movePos(offs);
}

void OS::Core::opCall()
{
	StackFunction * stack_func = this->stack_func;
#if 1 // speed optimization
	OS_ASSERT(stack_func->opcodes.getPos() + 2 <= stack_func->opcodes.size);
	OS_BYTE * buf = stack_func->opcodes.cur;
	stack_func->opcodes.cur += 2;
	int params = buf[0];
	int ret_values = buf[1];
#else
	int params = stack_func->opcodes.readByte();
	int ret_values = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(stack_values.count >= 2 + params);
	// insertValue(Value(), -params);
	call(params, ret_values, NULL, true);
}

void OS::Core::opSuperCall(int& break_with_ret_values)
{
	StackFunction * stack_func = this->stack_func;
#if 1 // speed optimization
	OS_ASSERT(stack_func->opcodes.getPos() + 2 <= stack_func->opcodes.size);
	OS_BYTE * buf = stack_func->opcodes.cur;
	stack_func->opcodes.cur += 2;
	int params = buf[0];
	int ret_values = buf[1];
#else
	int params = stack_func->opcodes.readByte();
	int ret_values = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(stack_values.count >= 2+params);
	OS_ASSERT(stack_values.buf[stack_values.count-2-params].type == OS_VALUE_TYPE_NULL);
	OS_ASSERT(stack_values.buf[stack_values.count-1-params].type == OS_VALUE_TYPE_NULL);

	GCFunctionValue * func_value = stack_func->func;
	if(stack_func->self_for_proto && func_value->name){
		GCValue * proto = stack_func->self_for_proto->prototype;
		if(stack_func->self_for_proto->is_object_instance){
			proto = proto ? proto->prototype : NULL;
		}
		if(proto){
			bool prototype_enabled = true;
			Value func;
			if(getPropertyValue(func, proto, 
				PropertyIndex(func_value->name, PropertyIndex::KeepStringIndex()), prototype_enabled)
				&& func.isFunction())
			{
				bool is_constructor = func_value->name == strings->__construct.string;
				stack_values.buf[stack_values.count-2-params] = func;
				stack_values.buf[stack_values.count-1-params] = stack_func->self;
				int func_ret_values = call(params, is_constructor && !ret_values ? 1 : ret_values, proto);
				if(is_constructor){
					GCValue * new_self = stack_values.lastElement().getGCValue();
					if(!new_self){
						int cur_ret_values = 0;
						int ret_values = stack_func->need_ret_values;
						ret_values = syncRetValues(ret_values, cur_ret_values);
						// OS_ASSERT(stack_values.count == stack_func->bottom_stack_pos + ret_values);
						// stack_func->opcodes_pos = opcodes.getPos();
						OS_ASSERT(call_stack_funcs.count > 0 && &call_stack_funcs[call_stack_funcs.count-1] == stack_func);
						call_stack_funcs.count--;
						int caller_stack_pos = stack_func->caller_stack_pos;
						clearStackFunction(stack_func);
						removeStackValues(caller_stack_pos+1, stack_values.count - ret_values - caller_stack_pos);
						reloadStackFunctionCache();
						break_with_ret_values = ret_values;
						return;
					}
					if(new_self != stack_func->self.getGCValue()){
						stack_func->self = new_self;
						stack_func->self_for_proto = new_self;
					}
				}
				syncRetValues(ret_values, func_ret_values);
				return;
			}
		}
	}
	pop(2+params);
	syncRetValues(ret_values, 0);
}

void OS::Core::opTailCall(int& out_ret_values)
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
	int params = *stack_func->opcodes.cur++; // readByte();
	int ret_values = stack_func->need_ret_values;
	OS_ASSERT(stack_values.count >= 1 + params);
	Value func_value = stack_values[stack_values.count-1-params];
	OS_ASSERT(call_stack_funcs.count > 0 && &call_stack_funcs[call_stack_funcs.count-1] == stack_func);
	switch(func_value.type){
	case OS_VALUE_TYPE_CFUNCTION:
	case OS_VALUE_TYPE_OBJECT:
		// pushNull();
		// moveStackValue(-1, -params);
		insertValue(Value(), -params);
		call(params, ret_values);
		break;

	case OS_VALUE_TYPE_FUNCTION:
		{
			call_stack_funcs.count--;
			int caller_stack_pos = stack_func->caller_stack_pos;
			clearStackFunction(stack_func);
			removeStackValues(caller_stack_pos+1, stack_values.count - 1-params - caller_stack_pos);
			enterFunction(func_value.v.func, NULL, NULL, params, 1, ret_values);
			return;
		}

	default:
		// TODO: warn or error here???
		pop(1+params);
		ret_values = syncRetValues(ret_values, 0);
	}
	OS_ASSERT(stack_values.count == stack_func->stack_pos + ret_values);
	// stack_func->opcodes_pos = opcodes.getPos();
	OS_ASSERT(call_stack_funcs.count > 0 && &call_stack_funcs[call_stack_funcs.count-1] == stack_func);
	call_stack_funcs.count--;
	int caller_stack_pos = stack_func->caller_stack_pos;
	clearStackFunction(stack_func);
	removeStackValues(caller_stack_pos+1, stack_values.count - ret_values - caller_stack_pos);
	reloadStackFunctionCache();
	out_ret_values = ret_values;
}

void OS::Core::opCallMethod()
{
	StackFunction * stack_func = this->stack_func;
#if 1 // speed optimization
	OS_ASSERT(stack_func->opcodes.getPos() + 2 <= stack_func->opcodes.size);
	OS_BYTE * buf = stack_func->opcodes.cur;
	stack_func->opcodes.cur += 2;
	int params = buf[0];
	int ret_values = buf[1];
#else
	int params = stack_func->opcodes.readByte();
	int ret_values = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(stack_values.count >= 2 + params);
	Value table_value = stack_values[stack_values.count-2-params];
	pushPropertyValue(table_value, PropertyIndex(stack_values[stack_values.count-1-params]), true, true, true, false);
#if 1 // speed optimization
	stack_values[stack_values.count-2-params-1] = stack_values.lastElement();
	stack_values[stack_values.count-2-params-0] = table_value;
	stack_values.count--;

	call(params, ret_values, NULL, true);
#else
	pushValue(table_value);
	moveStackValues(-2, 2, -2-params);
	call(params, ret_values);
	removeStackValues(-2-ret_values, 2);
	return false;
#endif
}

void OS::Core::opTailCallMethod(int& out_ret_values)
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
	int params = *stack_func->opcodes.cur++; // readByte();
	int ret_values = stack_func->need_ret_values;
	OS_ASSERT(stack_values.count >= 2 + params);
	Value table_value = stack_values[stack_values.count-2-params];
	pushPropertyValue(table_value, Core::PropertyIndex(stack_values[stack_values.count-1-params]), true, true, true, false);
	Value func_value = stack_values.lastElement();
	GCValue * self = stack_func->self.getGCValue();
	GCValue * self_for_proto = stack_func->self_for_proto;
	GCValue * call_self = table_value.getGCValue();
	if(call_self && (!self || self->prototype != call_self)){
		self = call_self;
		self_for_proto = call_self;
	}
	pushValue(self);
	moveStackValues(-2, 2, -2-params);

	OS_ASSERT(call_stack_funcs.count > 0 && &call_stack_funcs[call_stack_funcs.count-1] == stack_func);
	int caller_stack_pos;
	switch(func_value.type){
	case OS_VALUE_TYPE_CFUNCTION:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
		call(params, ret_values);
		removeStackValues(-2-ret_values, 2);
		break;

	case OS_VALUE_TYPE_FUNCTION:
		call_stack_funcs.count--;
		caller_stack_pos = stack_func->caller_stack_pos;
		clearStackFunction(stack_func);
		removeStackValues(caller_stack_pos+1, stack_values.count - 4-params - caller_stack_pos);
		enterFunction(func_value.v.func, self, self_for_proto, params, 4, ret_values);
		return;

	default:
		// TODO: warn or error here???
		pop(4+params);
		ret_values = syncRetValues(ret_values, 0);
	}
	OS_ASSERT(stack_values.count == stack_func->stack_pos + ret_values);
	// stack_func->opcodes_pos = opcodes.getPos();
	OS_ASSERT(call_stack_funcs.count > 0 && &call_stack_funcs[call_stack_funcs.count-1] == stack_func);
	call_stack_funcs.count--;
	caller_stack_pos = stack_func->caller_stack_pos;
	clearStackFunction(stack_func);
	removeStackValues(caller_stack_pos+1, stack_values.count - ret_values - caller_stack_pos);
	reloadStackFunctionCache();
	out_ret_values = ret_values;
}

int OS::Core::opReturn()
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
	int cur_ret_values = *stack_func->opcodes.cur++; // readByte();
	int ret_values = stack_func->need_ret_values;
	ret_values = syncRetValues(ret_values, cur_ret_values);
	OS_ASSERT(stack_values.count == stack_func->stack_pos + ret_values);
	// stack_func->opcodes_pos = opcodes.getPos();
	OS_ASSERT(call_stack_funcs.count > 0 && &call_stack_funcs[call_stack_funcs.count-1] == stack_func);
	call_stack_funcs.count--;
	int caller_stack_pos = stack_func->caller_stack_pos;
	clearStackFunction(stack_func);
	removeStackValues(caller_stack_pos+1, stack_values.count - ret_values - caller_stack_pos);
	reloadStackFunctionCache();
	return ret_values;
}

int OS::Core::opReturnAuto()
{
	StackFunction * stack_func = this->stack_func;
	int cur_ret_values = 0;
	int ret_values = stack_func->need_ret_values;
	GCFunctionValue * func_value = stack_func->func;
	if(ret_values > 0 && func_value->name && func_value->name == strings->__construct.string){
		pushValue(stack_func->self);
		cur_ret_values = 1;
	}
	ret_values = syncRetValues(ret_values, cur_ret_values);
	// OS_ASSERT(stack_values.count == stack_func->stack_pos + ret_values);
	// stack_func->opcodes_pos = opcodes.getPos();
	OS_ASSERT(call_stack_funcs.count > 0 && &call_stack_funcs[call_stack_funcs.count-1] == stack_func);
	call_stack_funcs.count--;
	int caller_stack_pos = stack_func->caller_stack_pos;
	clearStackFunction(stack_func);
	removeStackValues(caller_stack_pos+1, stack_values.count - ret_values - caller_stack_pos);
	reloadStackFunctionCache();
	return ret_values;
}

void OS::Core::opGetProperty(bool auto_create)
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
	int ret_values = *stack_func->opcodes.cur++; // readByte();
	OS_ASSERT(stack_values.count >= 2);
	pushPropertyValue(stack_values.buf[stack_values.count - 2], 
		PropertyIndex(stack_values.buf[stack_values.count - 1]), true, true, true, auto_create);
	removeStackValues(-3, 2);
	// OS_ASSERT(ret_values == 1);
	syncRetValues(ret_values, 1);
}

void OS::Core::opGetThisPropertyByString()
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
	int ret_values = *stack_func->opcodes.cur++; // readByte();
	int i = stack_func->opcodes.readUVariable();
	OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_strings);
	OS_ASSERT(stack_func_prog_strings[i]->type == OS_VALUE_TYPE_STRING);
	pushPropertyValue(stack_func->self, 
		PropertyIndex(stack_func_prog_strings[i], PropertyIndex::KeepStringIndex()), true, true, true, false);
	// OS_ASSERT(ret_values == 1);
	syncRetValues(ret_values, 1);
}

void OS::Core::opGetPropertyByLocals(bool auto_create)
{
	StackFunction * stack_func = this->stack_func;
#if 1 // speed optimization
	OS_ASSERT(stack_func->opcodes.getPos() + 3 <= stack_func->opcodes.size);
	OS_BYTE * buf = stack_func->opcodes.cur;
	stack_func->opcodes.cur += 3;
	int ret_values = buf[0];
	int local_1 = buf[1];
	int local_2 = buf[2];
#else
	int ret_values = stack_func->opcodes.readByte();
	int local_1 = stack_func->opcodes.readByte();
	int local_2 = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(local_1 < num_stack_func_locals && local_2 < num_stack_func_locals);
#if 1 // inline function for speed optimization
	const bool anonymous_getter_enabled = true, named_getter_enabled = true, prototype_enabled = true;
	Value * stack_func_locals = this->stack_func_locals;
	Value table_value = stack_func_locals[local_1];
	PropertyIndex index(stack_func_locals[local_2]);
	switch(table_value.type){
	case OS_VALUE_TYPE_NULL:
		break;

	case OS_VALUE_TYPE_BOOL:
	case OS_VALUE_TYPE_NUMBER:
		pushPropertyValueForPrimitive(table_value, index, anonymous_getter_enabled, named_getter_enabled, prototype_enabled, auto_create);
		syncRetValues(ret_values, 1);
		return;

	case OS_VALUE_TYPE_STRING:
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_FUNCTION:
	case OS_VALUE_TYPE_CFUNCTION:
		for(GCValue * self = table_value.v.value;;){
#if 1  // inline function for speed optimization
			for(;;){
				if(self->type == OS_VALUE_TYPE_ARRAY && index.index.type == OS_VALUE_TYPE_NUMBER){
					OS_ASSERT(dynamic_cast<GCArrayValue*>(self));
					int i = (int)index.index.v.number;
					if((i >= 0 || (i += ((GCArrayValue*)self)->values.count) >= 0) && i < ((GCArrayValue*)self)->values.count){
						pushValue(((GCArrayValue*)self)->values[i]);
					}else{
						pushNull();
					}
					syncRetValues(ret_values, 1);
					return;
				}
				Property * prop;
				Table * table = self->table;
				if(table && (prop = table->get(index))){
					pushValue(prop->value);
					syncRetValues(ret_values, 1);
					return;
				}
				if(prototype_enabled){
					GCValue * cur_value = self;
					while(cur_value->prototype){
						cur_value = cur_value->prototype;
						Table * cur_table = cur_value->table;
						if(cur_table && (prop = cur_table->get(index))){
							pushValue(prop->value);
							syncRetValues(ret_values, 1);
							return;
						}
					}
				}
				if(index.index.type == OS_VALUE_TYPE_STRING && strings->syntax_prototype == index.index.v.string){
					pushValue(self->prototype);
					syncRetValues(ret_values, 1);
					return;
				}
				if(self->type == OS_VALUE_TYPE_ARRAY){
					OS_ASSERT(dynamic_cast<GCArrayValue*>(self));
					OS_NUMBER number;
					if(isValueNumber(index.index, &number)){
						int i = (int)number;
						if((i >= 0 || (i += ((GCArrayValue*)self)->values.count) >= 0) && i < ((GCArrayValue*)self)->values.count){
							pushValue(((GCArrayValue*)self)->values[i]);
						}else{
							pushNull();
						}
					}else{
						pushNull();
					}
					syncRetValues(ret_values, 1);
					return;
				}
				break;
			}
#else
			Value value
			if(getPropertyValue(value, table_value, index, prototype_enabled)){
				pushValue(value);
				syncRetValues(ret_values, 1);
				return;
			}
#endif
			if(!hasSpecialPrefix(index.index)){
				Value value;
				if(index.index.type == OS_VALUE_TYPE_STRING){
					const void * buf1 = strings->__getAt.toChar();
					int size1 = strings->__getAt.getDataSize();
					const void * buf2 = index.index.v.string->toChar();
					int size2 = index.index.v.string->getDataSize();
					GCStringValue * getter_name = newStringValue(buf1, size1, buf2, size2);
					if(getPropertyValue(value, table_value, PropertyIndex(getter_name, PropertyIndex::KeepStringIndex()), prototype_enabled)){
						pushValue(value);
						pushValue(self);
						call(0, 1);
						syncRetValues(ret_values, 1);
						return;
					}
				}
				if(getPropertyValue(value, table_value, PropertyIndex(strings->__get, PropertyIndex::KeepStringIndex()), prototype_enabled)){
					// auto_create = false;
					if(value.type == OS_VALUE_TYPE_OBJECT){
						table_value = value.v.value;
						continue;
					}
					pushValue(value);
					pushValue(self);
					pushValue(index.index);
					if(!auto_create){
						call(1, 1);
					}else{
						pushBool(true);
						call(2, 1);
					}
					if(auto_create && stack_values.lastElement().type == OS_VALUE_TYPE_NULL){
						pop();
						setPropertyValue(self, index, Value(pushObjectValue()), false, false); 
					}
					syncRetValues(ret_values, 1);
					return;
				}
			}
			if(auto_create){
				setPropertyValue(self, index, Value(pushObjectValue()), false, false); 
				syncRetValues(ret_values, 1);
				return;
			}
			break;
		}
		break;
	}
	pushNull();
#else
	pushPropertyValue(stack_func_locals[local_1], 
		PropertyIndex(stack_func_locals[local_2]), true, true, true, auto_create);
#endif
	// OS_ASSERT(ret_values == 1);
	syncRetValues(ret_values, 1);
}

void OS::Core::opGetPropertyByLocalAndNumber(bool auto_create)
{
	StackFunction * stack_func = this->stack_func;
#if 1 // speed optimization
	OS_ASSERT(stack_func->opcodes.getPos() + 2 <= stack_func->opcodes.size);
	OS_BYTE * buf = stack_func->opcodes.cur;
	stack_func->opcodes.cur += 2;
	int ret_values = buf[0];
	int local_1 = buf[1];
#else
	int ret_values = stack_func->opcodes.readByte();
	int local_1 = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(local_1 < num_stack_func_locals);
	int number_index = stack_func->opcodes.readUVariable();
	OS_ASSERT(number_index >= 0 && number_index < stack_func->func->prog->num_numbers);
	pushPropertyValue(stack_func_locals[local_1], 
		PropertyIndex(stack_func_prog_numbers[number_index]), true, true, true, auto_create);
	// OS_ASSERT(ret_values == 1);
	syncRetValues(ret_values, 1);
}

void OS::Core::opSetProperty()
{
	OS_ASSERT(stack_values.count >= 3);
	setPropertyValue(stack_values.buf[stack_values.count - 2], 
		PropertyIndex(stack_values.buf[stack_values.count - 1]), 
		stack_values.buf[stack_values.count - 3], true, true);
	pop(3);
}

void OS::Core::opSetPropertyByLocals(bool auto_create)
{
	OS_ASSERT(stack_values.count >= 1);
#if 1 // speed optimization
	OS_ASSERT(stack_func->opcodes.getPos() + 2 <= stack_func->opcodes.size);
	OS_BYTE * buf = stack_func->opcodes.cur;
	stack_func->opcodes.cur += 2;
	int local_1 = buf[0];
	int local_2 = buf[1];
#else
	int local_1 = stack_func->opcodes.readByte();
	int local_2 = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(local_1 < num_stack_func_locals && local_2 < num_stack_func_locals);
	if(auto_create && stack_func_locals[local_1].type == OS_VALUE_TYPE_NULL){
		stack_func_locals[local_1] = newObjectValue();
	}
	setPropertyValue(stack_func_locals[local_1], 
		PropertyIndex(stack_func_locals[local_2]), 
		stack_values.lastElement(), true, true);
	pop();
}

void OS::Core::opGetSetPropertyByLocals(bool auto_create)
{
	OS_ASSERT(stack_values.count >= 1);
#if 1 // speed optimization
	OS_ASSERT(stack_func->opcodes.getPos() + 4 <= stack_func->opcodes.size);
	OS_BYTE * buf = stack_func->opcodes.cur;
	stack_func->opcodes.cur += 4;
	int local_1 = buf[0];
	int local_2 = buf[1];
#else
	int local_1 = stack_func->opcodes.readByte();
	int local_2 = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(local_1 < num_stack_func_locals && local_2 < num_stack_func_locals);
	pushPropertyValue(stack_func_locals[local_1], 
		PropertyIndex(stack_func_locals[local_2]), true, true, true, auto_create);
#if 1 // speed optimization
	local_1 = buf[2];
	local_2 = buf[3];
#else
	local_1 = stack_func->opcodes.readByte();
	local_2 = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(local_1 < num_stack_func_locals && local_2 < num_stack_func_locals);
	if(auto_create && stack_func_locals[local_1].type == OS_VALUE_TYPE_NULL){
		stack_func_locals[local_1] = newObjectValue();
	}
	setPropertyValue(stack_func_locals[local_1], 
		PropertyIndex(stack_func_locals[local_2]), 
		stack_values.lastElement(), true, true);
	pop();
}

void OS::Core::opSetDim()
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
	int params = *stack_func->opcodes.cur++; // readByte();

	OS_ASSERT(stack_values.count >= 2 + params);
	moveStackValue(-2-params, -1-params); // put value to the first param
	params++;

	Value table_value = stack_values[stack_values.count-1-params];
	Value func;
	if(getPropertyValue(func, table_value, 
		PropertyIndex(params == 1 ? strings->__setempty : strings->__setdim, PropertyIndex::KeepStringIndex()), true)
		&& func.isFunction())
	{
#if 1 // speed optimization
		insertValue(func, -1-params);
		call(params, 0, NULL, true);
#else
		pushValue(func);
		pushValue(table_value);
		moveStackValues(-2, 2, -2-params); // put func value before params
		call(params, 0);
		removeStackValue(-1); // remove table_value
#endif
	}else{
		pop(params+1);
	}
}

void OS::Core::opExtends()
{
	OS_ASSERT(stack_values.count >= 2);
	Value right_value = stack_values.lastElement();
	switch(right_value.type){
	case OS_VALUE_TYPE_NULL:
		// null value has no prototype
		break;

	case OS_VALUE_TYPE_BOOL:
	case OS_VALUE_TYPE_NUMBER:
		break;

	case OS_VALUE_TYPE_STRING:
	case OS_VALUE_TYPE_ARRAY:
	case OS_VALUE_TYPE_OBJECT:
	case OS_VALUE_TYPE_FUNCTION:
		right_value.v.value->prototype = stack_values[stack_values.count-2].getGCValue();
		break;

	case OS_VALUE_TYPE_USERDATA:
	case OS_VALUE_TYPE_USERPTR:
	case OS_VALUE_TYPE_CFUNCTION:
		// TODO: warning???
		break;
	}
	removeStackValue(-2);
}

void OS::Core::opClone()
{
	OS_ASSERT(stack_values.count >= 1);
	pushCloneValue(stack_values.lastElement());
	removeStackValue(-2);
}

void OS::Core::opDeleteProperty()
{
	OS_ASSERT(stack_values.count >= 2);
	deleteValueProperty(stack_values[stack_values.count-2], 
		stack_values.lastElement(), true, true, false);
	pop(2);
}

void OS::Core::opLogicAndOr1(bool is_and)
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 1);
	// Value value = stack_values.lastElement();
	if(valueToBool(stack_values.lastElement()) != is_and){
		// int offs = stack_func->opcodes.readInt16();
		OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
		int offs = (OS_INT8)stack_func->opcodes.cur[0];
		// stack_func->opcodes.movePos(offs);
		OS_ASSERT(stack_func->opcodes.getPos()+offs >= 0 && stack_func->opcodes.getPos()+offs <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += offs;
	}else{
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) >= 0);
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += sizeof(OS_INT32);
		// pop();
		--stack_values.count;
	}
}

void OS::Core::opLogicAndOr2(bool is_and)
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 1);
	// Value value = stack_values.lastElement();
	if(valueToBool(stack_values.lastElement()) != is_and){
		// int offs = stack_func->opcodes.readInt16();
		OS_ASSERT(stack_func->opcodes.getPos()+2 <= stack_func->opcodes.getSize());
		int offs = (OS_INT16)(stack_func->opcodes.cur[0] | (stack_func->opcodes.cur[1] << 8));
		// stack_func->opcodes.movePos(offs);
		OS_ASSERT(stack_func->opcodes.getPos()+offs >= 0 && stack_func->opcodes.getPos()+offs <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += offs;
	}else{
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) >= 0);
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += sizeof(OS_INT32);
		// pop();
		--stack_values.count;
	}
}

void OS::Core::opLogicAndOr4(bool is_and)
{
	StackFunction * stack_func = this->stack_func;
	OS_ASSERT(stack_values.count >= 1);
	// Value value = stack_values.lastElement();
	if(valueToBool(stack_values.lastElement()) != is_and){
		int offs = stack_func->opcodes.readInt32();
		// stack_func->opcodes.movePos(offs);
		OS_ASSERT(stack_func->opcodes.getPos()+offs >= 0 && stack_func->opcodes.getPos()+offs <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += offs;
	}else{
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) >= 0);
		OS_ASSERT(stack_func->opcodes.getPos()+(int)sizeof(OS_INT32) <= stack_func->opcodes.getSize());
		stack_func->opcodes.cur += sizeof(OS_INT32);
		// pop();
		--stack_values.count;
	}
}

void OS::Core::opSuper()
{
	StackFunction * stack_func = this->stack_func;
	if(stack_func->self_for_proto){
		GCValue * proto = stack_func->self_for_proto->prototype;
		if(stack_func->self_for_proto->is_object_instance){
			proto = proto ? proto->prototype : NULL;
		}					
		pushValue(proto);
	}else{
		pushNull();
	}
}

void OS::Core::opTypeOf()
{
	OS_ASSERT(stack_values.count >= 1);
	pushTypeOf(stack_values.lastElement());
	removeStackValue(-2);
}

void OS::Core::opValueOf()
{
	OS_ASSERT(stack_values.count >= 1);
	pushValueOf(stack_values.lastElement());
	removeStackValue(-2);
}

void OS::Core::opNumberOf()
{
	OS_ASSERT(stack_values.count >= 1);
	pushNumberOf(stack_values.lastElement());
	removeStackValue(-2);
}

void OS::Core::opStringOf()
{
	OS_ASSERT(stack_values.count >= 1);
	pushStringOf(stack_values.lastElement());
	removeStackValue(-2);
}

void OS::Core::opArrayOf()
{
	OS_ASSERT(stack_values.count >= 1);
	pushArrayOf(stack_values.lastElement());
	removeStackValue(-2);
}

void OS::Core::opObjectOf()
{
	OS_ASSERT(stack_values.count >= 1);
	pushObjectOf(stack_values.lastElement());
	removeStackValue(-2);
}

void OS::Core::opUserdataOf()
{
	OS_ASSERT(stack_values.count >= 1);
	pushUserdataOf(stack_values.lastElement());
	removeStackValue(-2);
}

void OS::Core::opFunctionOf()
{
	OS_ASSERT(stack_values.count >= 1);
	pushFunctionOf(stack_values.lastElement());
	removeStackValue(-2);
}

void OS::Core::opBooleanOf(bool b)
{
	OS_ASSERT(stack_values.count >= 1);
	stack_values.lastElement() = valueToBool(stack_values.lastElement()) == b;
}

void OS::Core::opIn()
{
	OS_ASSERT(stack_values.count >= 2);
	Core::GCValue * self = stack_values.lastElement().getGCValue();
	bool has_property = self && hasProperty(self, stack_values[stack_values.count-2], true, true, true);
	pop(2);
	pushBool(has_property);
}

void OS::Core::opIsPrototypeOf()
{
	OS_ASSERT(stack_values.count >= 2);
	bool ret = isValuePrototypeOf(stack_values[stack_values.count-2], stack_values.lastElement());
	pop(2);
	pushBool(ret);
}

void OS::Core::opIs()
{
	OS_ASSERT(stack_values.count >= 2);
	bool ret = isValueInstanceOf(stack_values[stack_values.count-2], stack_values.lastElement());
	pop(2);
	pushBool(ret);
}

void OS::Core::opLength()
{
	OS_ASSERT(stack_values.count >= 1);
	Value value = stack_values.lastElement();
	bool prototype_enabled = true;
	Value func;
	if(getPropertyValue(func, value, 
		PropertyIndex(strings->__len, PropertyIndex::KeepStringIndex()), prototype_enabled)
		&& func.isFunction())
	{
		pushValue(func);
		pushValue(value);
		call(0, 1);
	}else{
		pushNull();
	}
	removeStackValue(-2);
}

void OS::Core::opUnaryOperator(int opcode)
{
	OS_ASSERT(stack_values.count >= 1);
	pushOpResultValue(opcode, stack_values.lastElement());
	removeStackValue(-2);
}

/*
void OS::Core::opBinaryOperator(int opcode)
{
	OS_ASSERT(stack_values.count >= 2);
	pushOpResultValue(opcode, stack_values[stack_values.count-2], stack_values.lastElement());
	removeStackValues(-3, 2);
}
*/

void OS::Core::opBinaryOperatorByLocals()
{
	StackFunction * stack_func = this->stack_func;
#if 1 // speed optimization
	OS_ASSERT(stack_func->opcodes.getPos() + 3 <= stack_func->opcodes.size);
	OS_BYTE * buf = stack_func->opcodes.cur;
	stack_func->opcodes.cur += 3;
	int opcode = buf[0];
	int local_1 = buf[1];
	int local_2 = buf[2];
#else
	int opcode = stack_func->opcodes.readByte();
	int local_1 = stack_func->opcodes.readByte();
	int local_2 = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(local_1 < num_stack_func_locals && local_2 < num_stack_func_locals);
#if 0 // increase in speed is not detected
	Value * stack_func_locals = this->stack_func_locals;
	const Value& left_value = stack_func_locals[local_1];
	const Value& right_value = stack_func_locals[local_2];
	if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
		StackValues * stack_values = &this->stack_values;
		if(stack_values->capacity < stack_values->count+1){
			reserveStackValues(stack_values->count+1);
		}
		OS_NUMBER right_num;
		switch(opcode){
		case Program::OP_COMPARE:
			stack_values->buf[stack_values->count++] = left_value.v.number - right_value.v.number;
			return;
			
		case Program::OP_LOGIC_EQ:
			stack_values->buf[stack_values->count++] = left_value.v.number == right_value.v.number;
			return;

		case Program::OP_LOGIC_NE:
			stack_values->buf[stack_values->count++] = left_value.v.number != right_value.v.number;
			return;

		case Program::OP_LOGIC_GE:
			stack_values->buf[stack_values->count++] = left_value.v.number >= right_value.v.number;
			return;

		case Program::OP_LOGIC_LE:
			stack_values->buf[stack_values->count++] = left_value.v.number <= right_value.v.number;
			return;

		case Program::OP_LOGIC_GREATER:
			stack_values->buf[stack_values->count++] = left_value.v.number > right_value.v.number;
			return;

		case Program::OP_LOGIC_LESS:
			stack_values->buf[stack_values->count++] = left_value.v.number < right_value.v.number;
			return;

		case Program::OP_BIT_AND:
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number & (OS_INT)right_value.v.number;
			return;

		case Program::OP_BIT_OR:
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number | (OS_INT)right_value.v.number;
			return;

		case Program::OP_BIT_XOR:
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number ^ (OS_INT)right_value.v.number;
			return;

		case Program::OP_ADD: // +
			stack_values->buf[stack_values->count++] = left_value.v.number + right_value.v.number;
			return;

		case Program::OP_SUB: // -
			stack_values->buf[stack_values->count++] = left_value.v.number - right_value.v.number;
			return;

		case Program::OP_MUL: // *
			stack_values->buf[stack_values->count++] = left_value.v.number * right_value.v.number;
			return;

		case Program::OP_DIV: // /
			right_num = right_value.v.number;
			if(!right_num){
				errorDivisionByZero();
				stack_values->buf[stack_values->count++] = 0.0;
			}else{
				stack_values->buf[stack_values->count++] = left_value.v.number / right_num;
			}
			return;

		case Program::OP_MOD: // %
			right_num = right_value.v.number;
			if(!right_num){
				errorDivisionByZero();
				stack_values->buf[stack_values->count++] = 0.0;
			}else{
				stack_values->buf[stack_values->count++] = OS_MATH_MOD_OPERATOR(left_value.v.number, right_num);
			}
			return;

		case Program::OP_LSHIFT: // <<
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number << (OS_INT)right_value.v.number;
			return;

		case Program::OP_RSHIFT: // >>
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number >> (OS_INT)right_value.v.number;
			return;

		case Program::OP_POW: // **
			stack_values->buf[stack_values->count++] = OS_MATH_POW_OPERATOR((OS_FLOAT)left_value.v.number, (OS_FLOAT)right_value.v.number);
			return;
		}
	}
	pushOpResultValue(opcode, left_value, right_value);
#else
	pushOpResultValue(opcode, stack_func_locals[local_1], stack_func_locals[local_2]);
#endif
}

void OS::Core::opBinaryOperatorByLocalAndNumber()
{
	StackFunction * stack_func = this->stack_func;
#if 1 // speed optimization
	OS_ASSERT(stack_func->opcodes.getPos() + 2 <= stack_func->opcodes.size);
	OS_BYTE * buf = stack_func->opcodes.cur;
	stack_func->opcodes.cur += 2;
	int opcode = buf[0];
	int local_1 = buf[1];
#else
	int opcode = stack_func->opcodes.readByte();
	int local_1 = stack_func->opcodes.readByte();
#endif
	OS_ASSERT(local_1 < num_stack_func_locals);
	int number_index = stack_func->opcodes.readUVariable();
	OS_ASSERT(number_index >= 0 && number_index < stack_func->func->prog->num_numbers);
#if 1 // inline function for speed optimization
	const Value& left_value = stack_func_locals[local_1];
	if(left_value.type == OS_VALUE_TYPE_NUMBER){
		StackValues * stack_values = &this->stack_values;
		if(stack_values->capacity < stack_values->count+1){
			reserveStackValues(stack_values->count+1);
		}
		OS_NUMBER right_num;
		switch(opcode){
		case Program::OP_COMPARE:
			stack_values->buf[stack_values->count++] = left_value.v.number - stack_func_prog_numbers[number_index];
			return;
			
		case Program::OP_LOGIC_EQ:
			stack_values->buf[stack_values->count++] = left_value.v.number == stack_func_prog_numbers[number_index];
			return;

		case Program::OP_LOGIC_NE:
			stack_values->buf[stack_values->count++] = left_value.v.number != stack_func_prog_numbers[number_index];
			return;

		case Program::OP_LOGIC_GE:
			stack_values->buf[stack_values->count++] = left_value.v.number >= stack_func_prog_numbers[number_index];
			return;

		case Program::OP_LOGIC_LE:
			stack_values->buf[stack_values->count++] = left_value.v.number <= stack_func_prog_numbers[number_index];
			return;

		case Program::OP_LOGIC_GREATER:
			stack_values->buf[stack_values->count++] = left_value.v.number > stack_func_prog_numbers[number_index];
			return;

		case Program::OP_LOGIC_LESS:
			stack_values->buf[stack_values->count++] = left_value.v.number < stack_func_prog_numbers[number_index];
			return;

		case Program::OP_BIT_AND:
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number & (OS_INT)stack_func_prog_numbers[number_index];
			return;

		case Program::OP_BIT_OR:
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number | (OS_INT)stack_func_prog_numbers[number_index];
			return;

		case Program::OP_BIT_XOR:
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number ^ (OS_INT)stack_func_prog_numbers[number_index];
			return;

		case Program::OP_ADD: // +
			stack_values->buf[stack_values->count++] = left_value.v.number + stack_func_prog_numbers[number_index];
			return;

		case Program::OP_SUB: // -
			stack_values->buf[stack_values->count++] = left_value.v.number - stack_func_prog_numbers[number_index];
			return;

		case Program::OP_MUL: // *
			stack_values->buf[stack_values->count++] = left_value.v.number * stack_func_prog_numbers[number_index];
			return;

		case Program::OP_DIV: // /
			right_num = stack_func_prog_numbers[number_index];
			if(!right_num){
				errorDivisionByZero();
				stack_values->buf[stack_values->count++] = 0.0;
			}else{
				stack_values->buf[stack_values->count++] = left_value.v.number / right_num;
			}
			return;

		case Program::OP_MOD: // %
			right_num = stack_func_prog_numbers[number_index];
			if(!right_num){
				errorDivisionByZero();
				stack_values->buf[stack_values->count++] = 0.0;
			}else{
				stack_values->buf[stack_values->count++] = OS_MATH_MOD_OPERATOR(left_value.v.number, right_num);
			}
			return;

		case Program::OP_LSHIFT: // <<
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number << (OS_INT)stack_func_prog_numbers[number_index];
			return;

		case Program::OP_RSHIFT: // >>
			stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number >> (OS_INT)stack_func_prog_numbers[number_index];
			return;

		case Program::OP_POW: // **
			stack_values->buf[stack_values->count++] = OS_MATH_POW_OPERATOR((OS_FLOAT)left_value.v.number, (OS_FLOAT)stack_func_prog_numbers[number_index]);
			return;
		}
	}
#endif
	pushOpResultValue(opcode, stack_func_locals[local_1], stack_func_prog_numbers[number_index]);
}

void OS::Core::reloadStackFunctionCache()
{
	if(call_stack_funcs.count > 0){
		stack_func = &call_stack_funcs.lastElement();
		stack_func_locals = stack_func->locals->locals;
		num_stack_func_locals = stack_func->locals->num_locals;
		stack_func_env_index = stack_func->func->func_decl->num_params + VAR_ENV;
		stack_func_prog_numbers = stack_func->func->prog->const_numbers;
		stack_func_prog_strings = stack_func->func->prog->const_strings;
	}else{
		stack_func = NULL;
		stack_func_locals = NULL;
		num_stack_func_locals = 0;
		stack_func_env_index = 0;
		stack_func_prog_numbers = NULL;
		stack_func_prog_strings = NULL;
	}
}

int OS::Core::execute()
{
#ifdef OS_DEBUG
	allocator->checkNativeStackUsage(OS_TEXT("OS::Core::execute"));
#endif
	StackFunction * stack_func;
	StackValues * stack_values;
	OS_NUMBER right_num;
	int i, ret_values, ret_stack_funcs = call_stack_funcs.count-1;
#ifdef OS_INFINITE_LOOP_OPCODES
	for(int opcodes_executed = 0;; opcodes_executed++){
#else
	for(;;){
#endif
		// StackFunction * stack_func = &call_stack_funcs.lastElement(); // could be invalid because of stack resize
		OS_ASSERT(this->stack_values.count >= this->stack_func->stack_pos);
		// stack_func->opcode_offs = opcodes.pos; // used by debugger to show currect position if debug info present
		if(terminated
#ifdef OS_INFINITE_LOOP_OPCODES
			|| opcodes_executed >= OS_INFINITE_LOOP_OPCODES
#endif
			)
		{
			break;
		}
		OS_ASSERT(this->stack_func->opcodes.getPos()+1 <= this->stack_func->opcodes.getSize());
		Program::OpcodeType opcode = (Program::OpcodeType)*(stack_func = this->stack_func)->opcodes.cur++; // readByte();
		OS_PROFILE_BEGIN_OPCODE(opcode);
		switch(opcode){
		default:
			error(OS_E_ERROR, "Unknown opcode, program is corrupted!!!");
			allocator->setTerminated();
			break;

		case Program::OP_DEBUGGER:
			opDebugger();
			break;

		case Program::OP_PUSH_ONE:
			//opPushNumber();
			stack_values = &this->stack_values;
			if(stack_values->capacity < stack_values->count+1){
				reserveStackValues(stack_values->count+1);
			}
			stack_values->buf[stack_values->count++] = 1.0f;
			break;

		case Program::OP_PUSH_NUMBER_1:
			//opPushNumber();
			OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
			i = *stack_func->opcodes.cur++; // readByte();
			OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_numbers);
			stack_values = &this->stack_values;
			if(stack_values->capacity < stack_values->count+1){
				reserveStackValues(stack_values->count+1);
			}
			stack_values->buf[stack_values->count++] = stack_func_prog_numbers[i];
			// pushNumber(stack_func_prog_numbers[i]);
			break;

		case Program::OP_PUSH_NUMBER_BY_AUTO_INDEX:
			//opPushNumber();
			i = stack_func->opcodes.readUVariable();
			OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_numbers);
			stack_values = &this->stack_values;
			if(stack_values->capacity < stack_values->count+1){
				reserveStackValues(stack_values->count+1);
			}
			stack_values->buf[stack_values->count++] = stack_func_prog_numbers[i];
			// pushNumber(stack_func_prog_numbers[i]);
			break;

		case Program::OP_PUSH_STRING_1:
			{
				OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
				i = *stack_func->opcodes.cur++; // readByte();
				OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_strings);
				OS_ASSERT(stack_func_prog_strings[i]->type == OS_VALUE_TYPE_STRING);
				stack_values = &this->stack_values;
				if(stack_values->capacity < stack_values->count+1){
					reserveStackValues(stack_values->count+1);
				}
				Value& value = stack_values->buf[stack_values->count++];
				value.v.string = stack_func_prog_strings[i];
				value.type = OS_VALUE_TYPE_STRING;
				// opPushString();
				break;
			}

		case Program::OP_PUSH_STRING_BY_AUTO_INDEX:
			{
				i = stack_func->opcodes.readUVariable();
				OS_ASSERT(i >= 0 && i < stack_func->func->prog->num_strings);
				OS_ASSERT(stack_func_prog_strings[i]->type == OS_VALUE_TYPE_STRING);
				stack_values = &this->stack_values;
				if(stack_values->capacity < stack_values->count+1){
					reserveStackValues(stack_values->count+1);
				}
				Value& value = stack_values->buf[stack_values->count++];
				value.v.string = stack_func_prog_strings[i];
				value.type = OS_VALUE_TYPE_STRING;
				// opPushString();
				break;
			}

		case Program::OP_PUSH_NULL:
			pushNull();
			break;

		case Program::OP_PUSH_TRUE:
			pushBool(true);
			break;

		case Program::OP_PUSH_FALSE:
			pushBool(false);
			break;

		case Program::OP_PUSH_FUNCTION:
			opPushFunction();
			break;

		case Program::OP_PUSH_NEW_ARRAY:
			opPushArray();
			break;

		case Program::OP_PUSH_NEW_OBJECT:
			opPushObject();
			break;

		case Program::OP_OBJECT_SET_BY_AUTO_INDEX:
			opObjectSetByAutoIndex();
			break;

		case Program::OP_OBJECT_SET_BY_EXP:
			opObjectSetByExp();
			break;

		case Program::OP_OBJECT_SET_BY_INDEX:
			opObjectSetByIndex();
			break;

		case Program::OP_OBJECT_SET_BY_NAME:
			opObjectSetByName();
			break;

		case Program::OP_PUSH_ENV_VAR:
		case Program::OP_PUSH_ENV_VAR_AUTO_CREATE:
			opPushEnvVar(opcode == Program::OP_PUSH_ENV_VAR_AUTO_CREATE);
			break;

		case Program::OP_SET_ENV_VAR:
			opSetEnvVar();
			break;

		case Program::OP_PUSH_THIS:
			opPushThis();
			break;

		case Program::OP_PUSH_ARGUMENTS:
			opPushArguments();
			break;

		case Program::OP_PUSH_REST_ARGUMENTS:
			opPushRestArguments();
			break;

		case Program::OP_PUSH_LOCAL_VAR_1:
			OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
			i = *stack_func->opcodes.cur++; // readByte();
			OS_ASSERT(i < num_stack_func_locals);
#if 1 // inline function for speed optimization
			stack_values = &this->stack_values;
			if(stack_values->capacity < stack_values->count+1){
				reserveStackValues(stack_values->count+1);
			}
			stack_values->buf[stack_values->count++] = stack_func_locals[i];
#else
			pushValue(stack_func_locals[i]);
#endif
			break;

		case Program::OP_PUSH_LOCAL_VAR_BY_AUTO_INDEX:
			opPushLocalVar();
			break;

		case Program::OP_PUSH_LOCAL_VAR_AUTO_CREATE:
			opPushLocalVarAutoCreate();
			break;

		case Program::OP_SET_LOCAL_VAR_1:
			// inline function for speed optimization
			{
				// StackFunction * stack_func = this->stack_func;
				OS_ASSERT(this->stack_values.count >= 1);
				OS_ASSERT(stack_func->opcodes.getPos()+1 <= stack_func->opcodes.getSize());
				i = *stack_func->opcodes.cur++; // readByte();
				// Upvalues * func_upvalues = stack_func->locals;
				OS_ASSERT(i < num_stack_func_locals);
				stack_values = &this->stack_values;
				switch((stack_func_locals[i] = stack_values->buf[--stack_values->count]).type){
				case OS_VALUE_TYPE_FUNCTION:
					OS_ASSERT(dynamic_cast<GCFunctionValue*>(stack_func_locals[i].v.func));
					if(!stack_func_locals[i].v.func->name){
						stack_func_locals[i].v.func->name = stack_func->func->func_decl->locals[i].name.string;
					}
					break;

				case OS_VALUE_TYPE_CFUNCTION:
					OS_ASSERT(dynamic_cast<GCCFunctionValue*>(stack_func_locals[i].v.cfunc));
					if(!stack_func_locals[i].v.cfunc->name){
						stack_func_locals[i].v.cfunc->name = stack_func->func->func_decl->locals[i].name.string;
					}
					break;
				}
				// already removed
				// pop();
				break;
			}

		case Program::OP_SET_LOCAL_VAR:
			opSetLocalVar();
			break;

		case Program::OP_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCALS:
			opBinaryOperatorByLocals();
			opSetLocalVar();
			break;

		case Program::OP_SET_LOCAL_VAR_1_BY_BIN_OPERATOR_LOCAL_AND_NUMBER:
			// inline function for speed optimization
			{
				// StackFunction * stack_func = this->stack_func;
				OS_ASSERT(stack_func->opcodes.getPos() + 4 <= stack_func->opcodes.size);
				OS_BYTE * buf = stack_func->opcodes.cur;
				stack_func->opcodes.cur += 4;
				int opcode = buf[0];
				int local_1 = buf[1];
				OS_ASSERT(local_1 < num_stack_func_locals);
				int number_index = buf[2]; // stack_func->opcodes.readUVariable();
				OS_ASSERT(number_index >= 0 && number_index < stack_func->func->prog->num_numbers);
				stack_values = &this->stack_values;
				const Value& left_value = stack_func_locals[local_1];
				if(left_value.type == OS_VALUE_TYPE_NUMBER){
					if(stack_values->capacity < stack_values->count+1){
						reserveStackValues(stack_values->count+1);
					}
					switch(opcode){
					case Program::OP_COMPARE:
						stack_values->buf[stack_values->count++] = left_value.v.number - stack_func_prog_numbers[number_index];
						break;
			
					case Program::OP_LOGIC_EQ:
						stack_values->buf[stack_values->count++] = left_value.v.number == stack_func_prog_numbers[number_index];
						break;

					case Program::OP_LOGIC_NE:
						stack_values->buf[stack_values->count++] = left_value.v.number != stack_func_prog_numbers[number_index];
						break;

					case Program::OP_LOGIC_GE:
						stack_values->buf[stack_values->count++] = left_value.v.number >= stack_func_prog_numbers[number_index];
						break;

					case Program::OP_LOGIC_LE:
						stack_values->buf[stack_values->count++] = left_value.v.number <= stack_func_prog_numbers[number_index];
						break;

					case Program::OP_LOGIC_GREATER:
						stack_values->buf[stack_values->count++] = left_value.v.number > stack_func_prog_numbers[number_index];
						break;

					case Program::OP_LOGIC_LESS:
						stack_values->buf[stack_values->count++] = left_value.v.number < stack_func_prog_numbers[number_index];
						break;

					case Program::OP_BIT_AND:
						stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number & (OS_INT)stack_func_prog_numbers[number_index];
						break;

					case Program::OP_BIT_OR:
						stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number | (OS_INT)stack_func_prog_numbers[number_index];
						break;

					case Program::OP_BIT_XOR:
						stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number ^ (OS_INT)stack_func_prog_numbers[number_index];
						break;

					case Program::OP_ADD: // +
						stack_values->buf[stack_values->count++] = left_value.v.number + stack_func_prog_numbers[number_index];
						break;

					case Program::OP_SUB: // -
						stack_values->buf[stack_values->count++] = left_value.v.number - stack_func_prog_numbers[number_index];
						break;

					case Program::OP_MUL: // *
						stack_values->buf[stack_values->count++] = left_value.v.number * stack_func_prog_numbers[number_index];
						break;

					case Program::OP_DIV: // /
						right_num = stack_func_prog_numbers[number_index];
						if(!right_num){
							errorDivisionByZero();
							stack_values->buf[stack_values->count++] = 0.0;
						}else{
							stack_values->buf[stack_values->count++] = left_value.v.number / right_num;
						}
						break;

					case Program::OP_MOD: // %
						right_num = stack_func_prog_numbers[number_index];
						if(!right_num){
							errorDivisionByZero();
							stack_values->buf[stack_values->count++] = 0.0;
						}else{
							stack_values->buf[stack_values->count++] = OS_MATH_MOD_OPERATOR(left_value.v.number, right_num);
						}
						break;

					case Program::OP_LSHIFT: // <<
						stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number << (OS_INT)stack_func_prog_numbers[number_index];
						break;

					case Program::OP_RSHIFT: // >>
						stack_values->buf[stack_values->count++] = (OS_INT)left_value.v.number >> (OS_INT)stack_func_prog_numbers[number_index];
						break;

					case Program::OP_POW: // **
						stack_values->buf[stack_values->count++] = OS_MATH_POW_OPERATOR((OS_FLOAT)left_value.v.number, (OS_FLOAT)stack_func_prog_numbers[number_index]);
						break;

					default:
						pushOpResultValue(opcode, stack_func_locals[local_1], stack_func_prog_numbers[number_index]);
					}
				}else{
					pushOpResultValue(opcode, stack_func_locals[local_1], stack_func_prog_numbers[number_index]);
				}
				// opSetLocalVar();
				OS_ASSERT(this->stack_values.count >= 1);
				i = buf[3];
				// Upvalues * func_upvalues = stack_func->locals;
				OS_ASSERT(i < num_stack_func_locals);
				// stack_values = &this->stack_values;
				switch((stack_func_locals[i] = stack_values->buf[--stack_values->count]).type){
				case OS_VALUE_TYPE_FUNCTION:
					OS_ASSERT(dynamic_cast<GCFunctionValue*>(stack_func_locals[i].v.func));
					if(!stack_func_locals[i].v.func->name){
						stack_func_locals[i].v.func->name = stack_func->func->func_decl->locals[i].name.string;
					}
					break;

				case OS_VALUE_TYPE_CFUNCTION:
					OS_ASSERT(dynamic_cast<GCCFunctionValue*>(stack_func_locals[i].v.cfunc));
					if(!stack_func_locals[i].v.cfunc->name){
						stack_func_locals[i].v.cfunc->name = stack_func->func->func_decl->locals[i].name.string;
					}
					break;
				}
				// already removed
				// pop();
				break;
			}

		case Program::OP_SET_LOCAL_VAR_BY_BIN_OPERATOR_LOCAL_AND_NUMBER:
			opBinaryOperatorByLocalAndNumber();
			opSetLocalVar();
			break;

		case Program::OP_PUSH_UP_LOCAL_VAR:
			opPushUpvalue();
			break;

		case Program::OP_PUSH_UP_LOCAL_VAR_AUTO_CREATE:
			opPushUpvalueAutoCreate();
			break;

		case Program::OP_SET_UP_LOCAL_VAR:
			opSetUpvalue();
			break;

		case Program::OP_IF_NOT_JUMP_1:
			opIfJump1(false);
			break;

		case Program::OP_IF_NOT_JUMP_2:
			opIfJump2(false);
			break;

		case Program::OP_IF_NOT_JUMP_4:
			opIfJump4(false);
			break;

		case Program::OP_IF_JUMP_1:
			opIfJump1(true);
			break;

		case Program::OP_IF_JUMP_2:
			opIfJump2(true);
			break;

		case Program::OP_IF_JUMP_4:
			opIfJump4(true);
			break;

		case Program::OP_JUMP_1:
			OS_ASSERT(stack_func->opcodes.getPos() + *(OS_INT8*)stack_func->opcodes.cur >= 0);
			OS_ASSERT(stack_func->opcodes.getPos() + *(OS_INT8*)stack_func->opcodes.cur <= stack_func->opcodes.getSize());
			stack_func->opcodes.cur += *(OS_INT8*)stack_func->opcodes.cur;
			break;

		case Program::OP_JUMP_2:
			OS_ASSERT(stack_func->opcodes.getPos() + (OS_INT16)(stack_func->opcodes.cur[0] | (stack_func->opcodes.cur[1] << 8)) >= 0);
			OS_ASSERT(stack_func->opcodes.getPos() + (OS_INT16)(stack_func->opcodes.cur[0] | (stack_func->opcodes.cur[1] << 8)) <= stack_func->opcodes.getSize());
			stack_func->opcodes.cur += (OS_INT16)(stack_func->opcodes.cur[0] | (stack_func->opcodes.cur[1] << 8));
			break;

		case Program::OP_JUMP_4:
			opJump4();
			break;

		case Program::OP_CALL:
			opCall();
			break;

		case Program::OP_SUPER_CALL:
			OS_PROFILE_END_OPCODE(opcode); // we shouldn't profile call here
			opSuperCall(ret_values);
			if(ret_stack_funcs >= call_stack_funcs.count){
				OS_ASSERT(ret_stack_funcs == call_stack_funcs.count);
				return ret_values;
			}
			continue;

		case Program::OP_TAIL_CALL:
			OS_PROFILE_END_OPCODE(opcode); // we shouldn't profile call here
			opTailCall(ret_values);
			if(ret_stack_funcs >= call_stack_funcs.count){
				OS_ASSERT(ret_stack_funcs == call_stack_funcs.count);
				return ret_values;
			}
			continue;

		case Program::OP_CALL_METHOD:
			OS_PROFILE_END_OPCODE(opcode); // we shouldn't profile call here
			opCallMethod();
			continue;

		case Program::OP_TAIL_CALL_METHOD:
			OS_PROFILE_END_OPCODE(opcode); // we shouldn't profile call here
			opTailCallMethod(ret_values);
			if(ret_stack_funcs >= call_stack_funcs.count){
				OS_ASSERT(ret_stack_funcs == call_stack_funcs.count);
				return ret_values;
			}
			continue;

		case Program::OP_RETURN:
			ret_values = opReturn();
			if(ret_stack_funcs >= call_stack_funcs.count){
				OS_ASSERT(ret_stack_funcs == call_stack_funcs.count);
				OS_PROFILE_END_OPCODE(opcode);
				return ret_values;
			}
			break;

		case Program::OP_RETURN_AUTO:
			ret_values = opReturnAuto();
			if(ret_stack_funcs >= call_stack_funcs.count){
				OS_ASSERT(ret_stack_funcs == call_stack_funcs.count);
				OS_PROFILE_END_OPCODE(opcode);
				return ret_values;
			}
			break;

		case Program::OP_GET_PROPERTY:
			opGetProperty(false);
			break;

		case Program::OP_GET_THIS_PROPERTY_BY_STRING:
			opGetThisPropertyByString();
			break;

		case Program::OP_GET_PROPERTY_AUTO_CREATE:
			opGetProperty(true);
			break;

		case Program::OP_GET_PROPERTY_BY_LOCALS:
			opGetPropertyByLocals(false);
			break;

		case Program::OP_GET_PROPERTY_BY_LOCAL_AND_NUMBER:
			opGetPropertyByLocalAndNumber(false);
			break;

		case Program::OP_SET_PROPERTY:
#if 1 // inline function for speed optimization
			OS_ASSERT(this->stack_values.count >= 3);
			stack_values = &this->stack_values;
			setPropertyValue(stack_values->buf[stack_values->count - 2], 
				PropertyIndex(stack_values->buf[stack_values->count - 1]), 
				stack_values->buf[stack_values->count - 3], true, true);
			OS_ASSERT(this->stack_values.count >= 3);
			// pop(3);
			stack_values->count -= 3;
#else
			opSetProperty();
#endif
			break;

		case Program::OP_SET_PROPERTY_BY_LOCALS_AUTO_CREATE:
			{
#if 0 // increase in speed is not detected
				OS_ASSERT(this->stack_values.count >= 1);
				OS_ASSERT(stack_func->opcodes.getPos() + 2 <= stack_func->opcodes.size);
				OS_BYTE * buf = stack_func->opcodes.cur;
				stack_func->opcodes.cur += 2;
				int local_1 = buf[0];
				int local_2 = buf[1];
				OS_ASSERT(local_1 < num_stack_func_locals && local_2 < num_stack_func_locals);
				Value * stack_func_locals = this->stack_func_locals;
				if(stack_func_locals[local_1].type == OS_VALUE_TYPE_NULL){
					stack_func_locals[local_1] = newObjectValue();
				}
				
				//setPropertyValue(stack_func_locals[local_1], PropertyIndex(stack_func_locals[local_2]), stack_values.lastElement(), true, true);
				PropertyIndex index(stack_func_locals[local_2]);
				Value value = stack_values->buf[--stack_values->count];
				Value table_value = stack_func_locals[local_1];
				switch(table_value.type){
				case OS_VALUE_TYPE_NULL:
					break;

				case OS_VALUE_TYPE_BOOL:
					// return setPropertyValue(prototypes[PROTOTYPE_BOOL], index, value, setter_enabled);
					break;

				case OS_VALUE_TYPE_NUMBER:
					// return setPropertyValue(prototypes[PROTOTYPE_NUMBER], index, value, setter_enabled);
					break;

				case OS_VALUE_TYPE_STRING:
					// return setPropertyValue(prototypes[PROTOTYPE_STRING], index, value, setter_enabled);
					// return;

				case OS_VALUE_TYPE_ARRAY:
				case OS_VALUE_TYPE_OBJECT:
				case OS_VALUE_TYPE_USERDATA:
				case OS_VALUE_TYPE_USERPTR:
				case OS_VALUE_TYPE_FUNCTION:
				case OS_VALUE_TYPE_CFUNCTION:
					setPropertyValue(table_value.v.value, index, value, true, true);
					break;
				}
				// already removed
				// pop();
#else
				opSetPropertyByLocals(true);
#endif
				break;
			}

		case Program::OP_GET_SET_PROPERTY_BY_LOCALS_AUTO_CREATE:
			opGetSetPropertyByLocals(true);
			break;

		case Program::OP_SET_DIM:
			opSetDim();
			break;

		case Program::OP_EXTENDS:
			opExtends();
			break;

		case Program::OP_CLONE:
			opClone();
			break;

		case Program::OP_DELETE_PROP:
			opDeleteProperty();
			break;

		case Program::OP_POP:
			// pop();
			OS_ASSERT(this->stack_values.count > 0);
			--this->stack_values.count;
			break;

		case Program::OP_LOGIC_AND_1:
			opLogicAndOr1(true);
			break;

		case Program::OP_LOGIC_AND_2:
			opLogicAndOr2(true);
			break;

		case Program::OP_LOGIC_AND_4:
			opLogicAndOr4(true);
			break;

		case Program::OP_LOGIC_OR_1:
			opLogicAndOr1(false);
			break;

		case Program::OP_LOGIC_OR_2:
			opLogicAndOr2(false);
			break;

		case Program::OP_LOGIC_OR_4:
			opLogicAndOr4(false);
			break;

		case Program::OP_SUPER:
			opSuper();
			break;

		case Program::OP_TYPE_OF:
			opTypeOf();
			break;

		case Program::OP_VALUE_OF:
			opValueOf();
			break;

		case Program::OP_NUMBER_OF:
			opNumberOf();
			break;

		case Program::OP_STRING_OF:
			opStringOf();
			break;

		case Program::OP_ARRAY_OF:
			opArrayOf();
			break;

		case Program::OP_OBJECT_OF:
			opObjectOf();
			break;

		case Program::OP_USERDATA_OF:
			opUserdataOf();
			break;

		case Program::OP_FUNCTION_OF:
			opFunctionOf();
			break;

		case Program::OP_LOGIC_BOOL:
		case Program::OP_LOGIC_NOT:
			opBooleanOf(opcode == Program::OP_LOGIC_BOOL);
			break;

		case Program::OP_IN:
			opIn();
			break;

		case Program::OP_ISPROTOTYPEOF:
			opIsPrototypeOf();
			break;

		case Program::OP_IS:
			opIs();
			break;

		case Program::OP_LENGTH:
			opLength();
			break;

		case Program::OP_BIT_NOT:
		case Program::OP_PLUS:
		case Program::OP_NEG:
			opUnaryOperator(opcode);
			break;

		case Program::OP_BIN_OPERATOR_BY_LOCALS:
			opBinaryOperatorByLocals();
			break;

		case Program::OP_BIN_OPERATOR_BY_LOCAL_AND_NUMBER:
			opBinaryOperatorByLocalAndNumber();
			break;

		case Program::OP_CONCAT:
		case Program::OP_LOGIC_PTR_EQ:
		case Program::OP_LOGIC_PTR_NE:
		case Program::OP_LOGIC_EQ:
		case Program::OP_LOGIC_NE:
		case Program::OP_LOGIC_GE:
		case Program::OP_LOGIC_LE:
		case Program::OP_LOGIC_GREATER:
		case Program::OP_LOGIC_LESS:
		case Program::OP_BIT_AND:
		case Program::OP_BIT_OR:
		case Program::OP_BIT_XOR:
		case Program::OP_ADD: // +
		case Program::OP_SUB: // -
		case Program::OP_MUL: // *
		case Program::OP_DIV: // /
		case Program::OP_MOD: // %
		case Program::OP_LSHIFT: // <<
		case Program::OP_RSHIFT: // >>
		case Program::OP_POW: // **
			// opBinaryOperator(opcode);
			{
				OS_ASSERT(this->stack_values.count >= 2);
				stack_values = &this->stack_values;
#if 1 // speed optimization
				const Value& left_value = stack_values->buf[stack_values->count-2];
				const Value& right_value = stack_values->buf[stack_values->count-1];
				if(left_value.type == OS_VALUE_TYPE_NUMBER && right_value.type == OS_VALUE_TYPE_NUMBER){
					if(stack_values->capacity < stack_values->count+1){
						reserveStackValues(stack_values->count+1);
					}
					switch(opcode){
					case Program::OP_COMPARE:
						stack_values->buf[--stack_values->count - 1] = left_value.v.number - right_value.v.number;
						break;
			
					case Program::OP_LOGIC_EQ:
						stack_values->buf[--stack_values->count - 1] = left_value.v.number == right_value.v.number;
						break;

					case Program::OP_LOGIC_NE:
						stack_values->buf[--stack_values->count - 1] = left_value.v.number != right_value.v.number;
						break;

					case Program::OP_LOGIC_GE:
						stack_values->buf[--stack_values->count - 1] = left_value.v.number >= right_value.v.number;
						break;

					case Program::OP_LOGIC_LE:
						stack_values->buf[--stack_values->count - 1] = left_value.v.number <= right_value.v.number;
						break;

					case Program::OP_LOGIC_GREATER:
						stack_values->buf[--stack_values->count - 1] = left_value.v.number > right_value.v.number;
						break;

					case Program::OP_LOGIC_LESS:
						stack_values->buf[--stack_values->count - 1] = left_value.v.number < right_value.v.number;
						break;

					case Program::OP_BIT_AND:
						stack_values->buf[--stack_values->count - 1] = (OS_INT)left_value.v.number & (OS_INT)right_value.v.number;
						break;

					case Program::OP_BIT_OR:
						stack_values->buf[--stack_values->count - 1] = (OS_INT)left_value.v.number | (OS_INT)right_value.v.number;
						break;

					case Program::OP_BIT_XOR:
						stack_values->buf[--stack_values->count - 1] = (OS_INT)left_value.v.number ^ (OS_INT)right_value.v.number;
						break;

					case Program::OP_ADD: // +
						stack_values->buf[--stack_values->count - 1] = left_value.v.number + right_value.v.number;
						break;

					case Program::OP_SUB: // -
						stack_values->buf[--stack_values->count - 1] = left_value.v.number - right_value.v.number;
						break;

					case Program::OP_MUL: // *
						stack_values->buf[--stack_values->count - 1] = left_value.v.number * right_value.v.number;
						break;

					case Program::OP_DIV: // /
						right_num = right_value.v.number;
						if(!right_num){
							errorDivisionByZero();
							stack_values->buf[--stack_values->count - 1] = 0.0;
						}else{
							stack_values->buf[--stack_values->count - 1] = left_value.v.number / right_num;
						}
						break;

					case Program::OP_MOD: // %
						right_num = right_value.v.number;
						if(!right_num){
							errorDivisionByZero();
							stack_values->buf[--stack_values->count - 1] = 0.0;
						}else{
							stack_values->buf[--stack_values->count - 1] = OS_MATH_MOD_OPERATOR(left_value.v.number, right_num);
						}
						break;

					case Program::OP_LSHIFT: // <<
						stack_values->buf[--stack_values->count - 1] = (OS_INT)left_value.v.number << (OS_INT)right_value.v.number;
						break;

					case Program::OP_RSHIFT: // >>
						stack_values->buf[--stack_values->count - 1] = (OS_INT)left_value.v.number >> (OS_INT)right_value.v.number;
						break;

					case Program::OP_POW: // **
						stack_values->buf[--stack_values->count - 1] = OS_MATH_POW_OPERATOR((OS_FLOAT)left_value.v.number, (OS_FLOAT)right_value.v.number);
						break;

					default:
						goto generic_bin_op;
					}
					break;
				}
generic_bin_op:
				pushOpResultValue(opcode, left_value, right_value);
#else
				pushOpResultValue(opcode, stack_values->buf[stack_values->count-2], stack_values->buf[stack_values->count-1]);
#endif
				OS_ASSERT(this->stack_values.count >= 3);
				stack_values->buf[stack_values->count-3] = stack_values->buf[stack_values->count-1];
				stack_values->count -= 2;
				// removeStackValues(-3, 2);
				break;
			}
		}
		OS_PROFILE_END_OPCODE(opcode);
	}
	for(;;){
		ret_values = opBreakFunction();
		if(ret_stack_funcs >= call_stack_funcs.count){
			OS_ASSERT(ret_stack_funcs == call_stack_funcs.count);
			return ret_values;
		}
	}
	return 0;
}

void OS::runOp(OS_EOpcode opcode)
{
	struct Lib
	{
		Core * core;

		void runBinaryOpcode(int opcode)
		{
			int count = core->stack_values.count;
			if(count < 2){
				core->pushNull();
				return;
			}
			Core::Value left_value = core->stack_values[count-2];
			Core::Value right_value = core->stack_values[count-1];
			// core->stack_values.count -= 2;
			core->pushOpResultValue(opcode, left_value, right_value);
			core->removeStackValues(-3, 2);
		}

		void runUnaryOpcode(int opcode)
		{
			int count = core->stack_values.count;
			if(count < 1){
				core->pushNull();
				return;
			}
			Core::Value value = core->stack_values[count-1];
			core->pushOpResultValue(opcode, value);
			core->removeStackValue(-2);
		}

	} lib = {core};
	switch(opcode){
	case OP_COMPARE:
		return lib.runBinaryOpcode(Core::Program::OP_COMPARE);

	case OP_LOGIC_PTR_EQ:	// ===
		return lib.runBinaryOpcode(Core::Program::OP_LOGIC_PTR_EQ);

	case OP_LOGIC_PTR_NE:	// !==
		return lib.runBinaryOpcode(Core::Program::OP_LOGIC_PTR_NE);

	case OP_LOGIC_EQ:		// ==
		return lib.runBinaryOpcode(Core::Program::OP_LOGIC_EQ);

	case OP_LOGIC_NE:		// !=
		return lib.runBinaryOpcode(Core::Program::OP_LOGIC_NE);

	case OP_LOGIC_GE:		// >=
		return lib.runBinaryOpcode(Core::Program::OP_LOGIC_GE);

	case OP_LOGIC_LE:		// <=
		return lib.runBinaryOpcode(Core::Program::OP_LOGIC_LE);

	case OP_LOGIC_GREATER:	// >
		return lib.runBinaryOpcode(Core::Program::OP_LOGIC_GREATER);

	case OP_LOGIC_LESS:		// <
		return lib.runBinaryOpcode(Core::Program::OP_LOGIC_LESS);

	case OP_BIT_AND:	// &
		return lib.runBinaryOpcode(Core::Program::OP_BIT_AND);

	case OP_BIT_OR:	// |
		return lib.runBinaryOpcode(Core::Program::OP_BIT_OR);

	case OP_BIT_XOR:	// ^
		return lib.runBinaryOpcode(Core::Program::OP_BIT_XOR);

	case OP_ADD: // +
		return lib.runBinaryOpcode(Core::Program::OP_ADD);

	case OP_SUB: // -
		return lib.runBinaryOpcode(Core::Program::OP_SUB);

	case OP_MUL: // *
		return lib.runBinaryOpcode(Core::Program::OP_MUL);

	case OP_DIV: // /
		return lib.runBinaryOpcode(Core::Program::OP_DIV);

	case OP_MOD: // %
		return lib.runBinaryOpcode(Core::Program::OP_MOD);

	case OP_LSHIFT: // <<
		return lib.runBinaryOpcode(Core::Program::OP_LSHIFT);

	case OP_RSHIFT: // >>
		return lib.runBinaryOpcode(Core::Program::OP_RSHIFT);

	case OP_POW: // **
		return lib.runBinaryOpcode(Core::Program::OP_POW);

	case OP_CONCAT: // ..
		return lib.runBinaryOpcode(Core::Program::OP_CONCAT);

	case OP_BIT_NOT:		// ~
		return lib.runUnaryOpcode(Core::Program::OP_BIT_NOT);

	case OP_PLUS:		// +
		return lib.runUnaryOpcode(Core::Program::OP_PLUS);

	case OP_NEG:			// -
		return lib.runUnaryOpcode(Core::Program::OP_NEG);

	case OP_LENGTH:		// #
		return lib.runUnaryOpcode(Core::Program::OP_LENGTH);

		/*
		case OP_LOGIC_BOOL:
		return lib.runUnaryOpcode(Core::Program::OP_LOGIC_BOOL);

		case OP_LOGIC_NOT:
		return lib.runUnaryOpcode(Core::Program::OP_LOGIC_NOT);

		case OP_VALUE_OF:
		return lib.runUnaryOpcode(Core::Program::OP_VALUE_OF);

		case OP_NUMBER_OF:
		return lib.runUnaryOpcode(Core::Program::OP_NUMBER_OF);

		case OP_STRING_OF:
		return lib.runUnaryOpcode(Core::Program::OP_STRING_OF);

		case OP_ARRAY_OF:
		return lib.runUnaryOpcode(Core::Program::OP_ARRAY_OF);

		case OP_OBJECT_OF:
		return lib.runUnaryOpcode(Core::Program::OP_OBJECT_OF);

		case OP_USERDATA_OF:
		return lib.runUnaryOpcode(Core::Program::OP_USERDATA_OF);

		case OP_FUNCTION_OF:
		return lib.runUnaryOpcode(Core::Program::OP_FUNCTION_OF);

		case OP_CLONE:
		return lib.runUnaryOpcode(Core::Program::OP_CLONE);
		*/
	}
	pushNull();
}

int OS::getLen(int offs)
{
	pushStackValue(offs);
	runOp(OP_LENGTH);
	return popInt();
}

void OS::getErrorHandler(int code)
{
	for(int i = 0; i < OS_ERROR_LEVELS; i++){
		if(code & (1<<i)){
			core->pushValue(core->error_handlers[i]);
			return;
		}
	}
	pushNull();
}

void OS::setErrorHandler(int code)
{
	bool returned = false;
	Core::Value func = core->getStackValue(-1);
	if(func.isFunction()){
		for(int i = 0; i < OS_ERROR_LEVELS; i++){
			if(code & (1<<i)){
				if(!returned){
					core->pushValue(core->error_handlers[i]);
					returned = true;
				}
				core->error_handlers[i] = func;
			}
		}
	}
	if(!returned){
		pushNull();
	}
	remove(-2);
}

void OS::setFunc(const FuncDef& def, bool anonymous_setter_enabled, bool named_setter_enabled, int closure_values, void * user_param)
{
	const FuncDef list[] = {def, {}};
	setFuncs(list, anonymous_setter_enabled, named_setter_enabled, closure_values, user_param);
}

void OS::setFuncs(const FuncDef * list, bool anonymous_setter_enabled, bool named_setter_enabled, int closure_values, void * user_param)
{
	for(; list->func; list++){
		pushStackValue(-1);
		pushString(list->name);
		// push closure_values for cfunction
		for(int i = 0; i < closure_values; i++){
			pushStackValue(-2-closure_values);
		}
		pushCFunction(list->func, closure_values, list->user_param ? list->user_param : user_param);
		setProperty(anonymous_setter_enabled, named_setter_enabled);
	}
}

void OS::setNumber(const NumberDef& def, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	const NumberDef list[] = {def, {}};
	setNumbers(list, anonymous_setter_enabled, named_setter_enabled);
}

void OS::setNumbers(const NumberDef * list, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	for(; list->name; list++){
		pushStackValue(-1);
		pushString(list->name);
		pushNumber(list->value);
		setProperty(anonymous_setter_enabled, named_setter_enabled);
	}
}

void OS::setString(const StringDef& def, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	const StringDef list[] = {def, {}};
	setStrings(list, anonymous_setter_enabled, named_setter_enabled);
}

void OS::setStrings(const StringDef * list, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	for(; list->name; list++){
		pushStackValue(-1);
		pushString(list->name);
		pushString(list->value);
		setProperty(anonymous_setter_enabled, named_setter_enabled);
	}
}

void OS::setNull(const NullDef& def, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	const NullDef list[] = {def, {}};
	setNulls(list, anonymous_setter_enabled, named_setter_enabled);
}

void OS::setNulls(const NullDef * list, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	for(; list->name; list++){
		pushStackValue(-1);
		pushString(list->name);
		pushNull();
		setProperty(anonymous_setter_enabled, named_setter_enabled);
	}
}

void OS::getObject(const OS_CHAR * name, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	pushStackValue(-1); // 2: copy parent object
	pushString(name);	// 3: index
	getProperty(anonymous_getter_enabled, named_getter_enabled, prototype_enabled); // 2: value
	if(isObject()){
		remove(-2);		// 1: remove parent object
		return;
	}
	pop();				// 1: parent object
	newObject();		// 2: result object
	pushStackValue(-2);	// 3: copy parent object
	pushString(name);	// 4: index
	pushStackValue(-3);	// 5: copy result object
	setProperty(anonymous_getter_enabled, named_getter_enabled); // 2: parent + result
	remove(-2);			// 1: remove parent object
}

void OS::getGlobalObject(const OS_CHAR * name, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	pushGlobals();
	getObject(name, anonymous_getter_enabled, named_getter_enabled, prototype_enabled);
}

void OS::getModule(const OS_CHAR * name, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	getGlobalObject(name, anonymous_getter_enabled, named_getter_enabled, prototype_enabled);
	pushStackValue(-1);
	pushGlobals();
	setPrototype();
}

void OS::getGlobal(const OS_CHAR * name, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	getGlobal(Core::String(this, name), anonymous_getter_enabled, named_getter_enabled, prototype_enabled);
}

void OS::getGlobal(const Core::String& name, bool anonymous_getter_enabled, bool named_getter_enabled, bool prototype_enabled)
{
	pushGlobals();
	pushString(name);
	getProperty(anonymous_getter_enabled, named_getter_enabled, prototype_enabled);
}

void OS::setGlobal(const OS_CHAR * name, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	setGlobal(Core::String(this, name), anonymous_setter_enabled, named_setter_enabled);
}

void OS::setGlobal(const Core::String& name, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	if(core->stack_values.count >= 1){
		Core::Value object = core->global_vars;
		Core::Value value = core->stack_values[core->stack_values.count - 1];
		Core::Value index = core->pushStringValue(name);
		core->setPropertyValue(object, Core::PropertyIndex(index), value, anonymous_setter_enabled, named_setter_enabled);
		pop(2);
	}
}

void OS::setGlobal(const FuncDef& func, bool anonymous_setter_enabled, bool named_setter_enabled)
{
	pushCFunction(func.func, func.user_param);
	setGlobal(func.name, anonymous_setter_enabled, named_setter_enabled);
}

void OS::initGlobalFunctions()
{
	struct Lib
	{
		static int print(OS * os, int params, int, int, void*)
		{
			int params_offs = os->getAbsoluteOffs(-params);
			for(int i = 0; i < params; i++){
				String str = os->toString(params_offs + i);
				os->printf("%s", str.toChar());
				if(i+1 < params){
					os->printf("\t");
				}
			}
			if(params > 0){
				os->printf("\n");
			}
			return 0;
		}

		static int echo(OS * os, int params, int, int, void*)
		{
			int params_offs = os->getAbsoluteOffs(-params);
			for(int i = 0; i < params; i++){
				String str = os->toString(params_offs + i);
				os->printf("%s", str.toChar());
			}
			return 0;
		}

		static int concat(OS * os, int params, int, int, void*)
		{
			if(params < 1){
				return 0;
			}
			OS::Core::StringBuffer buf(os);
			int params_offs = os->getAbsoluteOffs(-params);
			for(int i = 0; i < params; i++){
				buf += os->toString(params_offs + i);
			}
			os->core->pushValue(buf.toGCStringValue());
			return 1;
		}

		static int compileText(OS * os, int params, int, int need_ret_values, void*)
		{
			if(params < 1){
				return 0;
			}
			os->compile();
			return 1;
		}

		static int compileFile(OS * os, int params, int, int need_ret_values, void*)
		{
			if(params < 1){
				return 0;
			}
			bool required = params > 1 ? os->toBool(-params+1) : false;
			os->compileFile(os->toString(-params), required);
			return 1;
		}

		static int resolvePath(OS * os, int params, int, int, void*)
		{
			String filename = os->resolvePath(os->toString(-1));
			if(filename.getDataSize()){
				os->pushString(filename);
				return 1;
			}
			return 0;
		}

		static int debugBackTrace(OS * os, int params, int, int, void*)
		{
			switch(params){
			case 0:
				os->core->pushBackTrace(0, 10);
				break;

			case 1:
				os->core->pushBackTrace(os->toInt(-params), 10);
				break;

			default:
				os->core->pushBackTrace(os->toInt(-params), os->toInt(-params+1));
			}
			return 1;
		}

		static int terminate(OS * os, int params, int, int, void*)
		{
			os->setTerminated(true, os->toInt(-params));
			return 0;
		}

		static int setErrorHandler(OS * os, int params, int, int, void*)
		{
			switch(params){
			default:
				return 0;

			case 1:
				os->setErrorHandler();
				break;

			case 2:
				os->setErrorHandler(os->popInt());
				break;
			}
			return 1;
		}

		static int triggerError(OS * os, int params, int, int, void*)
		{
			int code = os->toInt(-params, OS_E_ERROR);
			String message = os->toString(-params+1, OS_TEXT("unknown error"));
			os->triggerError(code, message);
			return 0;
		}
	};
	FuncDef list[] = {
		{OS_TEXT("print"), Lib::print},
		{OS_TEXT("echo"), Lib::echo},
		{OS_TEXT("concat"), Lib::concat},
		{OS_TEXT("compileText"), Lib::compileText},
		{OS_TEXT("compileFile"), Lib::compileFile},
		{OS_TEXT("resolvePath"), Lib::resolvePath},
		{OS_TEXT("debugBackTrace"), Lib::debugBackTrace},
		{OS_TEXT("terminate"), Lib::terminate},
		{OS_TEXT("setErrorHandler"), Lib::setErrorHandler},
		{OS_TEXT("triggerError"), Lib::triggerError},
		{}
	};
	NumberDef numbers[] = {
		{OS_TEXT("E_ERROR"), OS_E_ERROR},
		{OS_TEXT("E_WARNING"), OS_E_WARNING},
		{OS_TEXT("E_NOTICE"), OS_E_NOTICE},
		{}
	};
	pushGlobals();
	setFuncs(list);
	setNumbers(numbers);
	pop();
}

void OS::initObjectClass()
{
	static intptr_t iterator_crc = (intptr_t)&iterator_crc;
	static intptr_t array_iterator_crc = (intptr_t)&array_iterator_crc;

	struct Object
	{
		static int rawget(OS * os, int params, int, int, void*)
		{
			bool anonymous_getter_enabled = false, named_getter_enabled = false, prototype_enabled = false;
			switch(params){
			case 0:
				break;

			default:
				os->pop(params-4);
				// no break

			case 4:
				prototype_enabled = os->popBool(false);
				// no break

			case 3:
				named_getter_enabled = os->popBool(false);
				// no break

			case 2:
				anonymous_getter_enabled = os->popBool(false);
				// no break

			case 1:
				os->getProperty(anonymous_getter_enabled, named_getter_enabled, prototype_enabled);
				return 1;
			}
			return 0;
		}

		static int rawset(OS * os, int params, int, int, void*)
		{
			bool anonymous_getter_enabled = false, named_getter_enabled = false;
			switch(params){
			case 0:
				break;

			default:
				os->pop(params-4);
				// no break

			case 4:
				named_getter_enabled = os->popBool(false);
				// no break

			case 3:
				anonymous_getter_enabled = os->popBool(false);
				// no break

			case 2:
				os->setProperty(anonymous_getter_enabled, named_getter_enabled);
			}
			return 0;
		}

		static int getValueId(OS * os, int params, int, int, void*)
		{
			os->pushNumber(os->getValueId(-params-1));
			return 1;
		}

		static int iteratorStep(OS * os, int params, int closure_values, int, void*)
		{
			OS_ASSERT(closure_values == 2);
			Core::Value self_var = os->core->getStackValue(-closure_values + 0);
			void * p = os->toUserdata(iterator_crc, -closure_values + 1);
			Core::Table::IteratorState * iter = (Core::Table::IteratorState*)p;
			if(iter->table){
				Core::GCValue * self = self_var.getGCValue();
				OS_ASSERT(self && iter->table == self->table);
				if(iter->prop){
					os->pushBool(true);
					os->core->pushValue(iter->prop->index);
					os->core->pushValue(iter->prop->value);
					iter->prop = iter->ascending ? iter->prop->next : iter->prop->prev;
					return 3;
				}
				iter->table->removeIterator(iter);
			}
			return 0;
		}

		static void iteratorStateDestructor(OS * os, void * data, void * user_param)
		{
			Core::Table::IteratorState * iter = (Core::Table::IteratorState*)data;
			if(iter->table){
				iter->table->removeIterator(iter);
			}
		}

		static int arrayIteratorStep(OS * os, int params, int closure_values, int, void*)
		{
			OS_ASSERT(closure_values == 2);
			Core::Value self_var = os->core->getStackValue(-closure_values + 0);
			int * pi = (int*)os->toUserdata(array_iterator_crc, -closure_values + 1);
			OS_ASSERT(self_var.type == OS_VALUE_TYPE_ARRAY && pi && pi[1]);
			if(pi[0] >= 0 && pi[0] < self_var.v.arr->values.count){
				os->pushBool(true);
				os->pushNumber(pi[0]);
				os->core->pushValue(self_var.v.arr->values[pi[0]]);
				pi[0] += pi[1];
				return 3;
			}
			return 0;
		}

		static int iterator(OS * os, int params, bool ascending)
		{
			Core::Value self_var = os->core->getStackValue(-params-1);
			if(self_var.type == OS_VALUE_TYPE_ARRAY){
				OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(self_var.v.arr));
				os->core->pushValue(self_var);

				int * pi = (int*)os->pushUserdata(array_iterator_crc, sizeof(int)*2);
				OS_ASSERT(pi);
				pi[0] = ascending ? 0 : self_var.v.arr->values.count-1;
				pi[1] = ascending ? 1 : -1;

				os->pushCFunction(arrayIteratorStep, 2);
				return 1;
			}
			Core::GCValue * self = self_var.getGCValue();
			if(self && self->table && self->table->count > 0){
				typedef Core::Table::IteratorState IteratorState;

				os->core->pushValue(self);

				IteratorState * iter = (IteratorState*)os->pushUserdata(iterator_crc, sizeof(IteratorState), iteratorStateDestructor);
				iter->table = NULL;
				iter->next = NULL;
				iter->prop = NULL;
				iter->ascending = ascending;
				self->table->addIterator(iter);

				os->pushCFunction(iteratorStep, 2);
				return 1;
			}
			return 0;
		}

		static int iterator(OS * os, int params, int closure_values, int, void*)
		{
			return iterator(os, params + closure_values, true);
		}

		static int reverseIterator(OS * os, int params, int closure_values, int, void*)
		{
			return iterator(os, params + closure_values, false);
		}

		static int smartSort(OS * os, int params, 
			int(*arrcomp)(OS*, const void*, const void*, void*), 
			int(*objcomp)(OS*, const void*, const void*, void*), void * user_param = NULL)
		{
			Core::Value self_var = os->core->getStackValue(-params-1);
			if(self_var.type == OS_VALUE_TYPE_ARRAY){
				OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(self_var.v.arr));
				if(arrcomp){
					os->core->sortArray(self_var.v.arr, arrcomp, user_param);
				}
				os->core->pushValue(self_var);
				return 1;
			}
			Core::GCValue * self = self_var.getGCValue();
			if(self){
				if(self->table && objcomp){
					os->core->sortTable(self->table, objcomp, user_param);
				}
				os->core->pushValue(self_var);
				return 1;
			}
			return 0;
		}

		static int sort(OS * os, int params, int, int, void*)
		{
			if(params < 1){
				return smartSort(os, params, Core::compareArrayValues, Core::comparePropValues);
			}
			String prop_name(os);
			if(os->core->isValueString(os->core->getStackValue(-params), &prop_name)){
				return smartSort(os, params, NULL, Core::compareObjectProperties, &prop_name);
			}
			return smartSort(os, params, Core::compareUserArrayValues, Core::compareUserPropValues);
		}

		static int rsort(OS * os, int params, int, int, void*)
		{
			if(params < 1){
				return smartSort(os, params, Core::compareArrayValuesReverse, Core::comparePropValuesReverse);
			}
			String prop_name(os);
			if(os->core->isValueString(os->core->getStackValue(-params), &prop_name)){
				return smartSort(os, params, NULL, Core::compareObjectPropertiesReverse, &prop_name);
			}
			return smartSort(os, params, Core::compareUserArrayValuesReverse, Core::compareUserPropValuesReverse);
		}

		static void userSortArrayByKeys(OS * os, Core::GCArrayValue * arr, int params, bool reverse)
		{
			Core::GCArrayValue * keys = os->core->pushArrayValue();
			os->vectorReserveCapacity(keys->values, arr->values.count OS_DBG_FILEPOS);
			keys->values.count = arr->values.count;
			for(int i = 0; i < arr->values.count; i++){
				keys->values[i] = i;
			}
			if(reverse){
				os->core->sortArray(keys, Core::compareUserArrayValuesReverse);
			}else{
				os->core->sortArray(keys, Core::compareUserArrayValues);
			}
			Vector<Core::Value> values;
			os->vectorReserveCapacity(values, arr->values.count OS_DBG_FILEPOS);
			OS_MEMCPY(values.buf, arr->values.buf, sizeof(Core::Value) * arr->values.count);
			values.count = arr->values.count;
			for(int i = 0; i < arr->values.count; i++){
				arr->values[i] = values[(int)os->core->valueToInt(keys->values[i])];
			}
			os->vectorClear(values);
			os->vectorClear(keys->values);
			os->pop();
		}

		static int ksort(OS * os, int params, int, int, void*)
		{
			Core::Value self_var = os->core->getStackValue(-params-1);
			if(self_var.type == OS_VALUE_TYPE_ARRAY){
				OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(self_var.v.arr));
				if(params > 0){
					userSortArrayByKeys(os, self_var.v.arr, params, false);
				}else{
					// os->core->sortArray(self_var.v.arr, arrcomp);
					// array is always sorted by keys so it's nothing to do
				}
				os->core->pushValue(self_var);
				return 1;
			}
			Core::GCValue * self = self_var.getGCValue();
			if(self){
				if(self->table){
					if(params > 0){
						os->core->sortTable(self->table, Core::compareUserPropKeys);
					}else{
						os->core->sortTable(self->table, Core::comparePropKeys);
					}
				}
				os->core->pushValue(self_var);
				return 1;
			}
			return 0;
		}

		static int krsort(OS * os, int params, int, int, void*)
		{
			Core::Value self_var = os->core->getStackValue(-params-1);
			if(self_var.type == OS_VALUE_TYPE_ARRAY){
				OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(self_var.v.arr));
				if(params > 0){
					userSortArrayByKeys(os, self_var.v.arr, params, true);
				}else{
					int mid = self_var.v.arr->values.count/2;
					for(int i = 0, j = self_var.v.arr->values.count-1; i < mid; i++, j--){
						Core::Value tmp = self_var.v.arr->values[i];
						self_var.v.arr->values[i] = self_var.v.arr->values[j];
						self_var.v.arr->values[j] = tmp;
					}
				}
				os->core->pushValue(self_var);
				return 1;
			}
			Core::GCValue * self = self_var.getGCValue();
			if(self){
				if(self->table){
					if(params > 0){
						os->core->sortTable(self->table, Core::compareUserPropKeysReverse);
					}else{
						os->core->sortTable(self->table, Core::comparePropKeysReverse);
					}
				}
				os->core->pushValue(self_var);
				return 1;
			}
			return 0;
		}

		static int length(OS * os, int params, int closure_values, int, void*)
		{
			Core::Value self_var = os->core->getStackValue(-params-closure_values-1);
			if(self_var.type == OS_VALUE_TYPE_ARRAY){
				OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(self_var.v.arr));
				os->pushNumber(self_var.v.arr->values.count);
				return 1;
			}
			Core::GCValue * self = self_var.getGCValue();
			if(self){
				os->pushNumber(self->table ? self->table->count : 0);
				return 1;
			}
			return 0;
		}

		static void appendQuotedString(Core::StringBuffer& buf, const Core::String& string)
		{
			buf += OS_TEXT("\"");
			int len = string.getLen();
			const OS_CHAR * str = string.toChar();
			for(int i = 0; i < len; i++, str++){
				switch(*str){
				case OS_TEXT('\"'): buf += OS_TEXT("\\\""); continue;
				case OS_TEXT('\r'): buf += OS_TEXT("\\r"); continue;
				case OS_TEXT('\n'): buf += OS_TEXT("\\n"); continue;
				case OS_TEXT('\t'): buf += OS_TEXT("\\t"); continue;
				case OS_TEXT('\\'): buf += OS_TEXT("\\\\"); continue;
				}
				if(*str < OS_TEXT(' ')){
					buf += OS_TEXT("0x");
					buf.append((OS_CHAR)'A' + ((int)*str >> 4));
					buf.append((OS_CHAR)'A' + ((int)*str >> 0));
					continue;
				}
				buf.append(*str);
			}
			buf += OS_TEXT("\"");
		}

		static int valueof(OS * os, int params, int closure_values, int, void*)
		{
			Core::Value self_var = os->core->getStackValue(-params-closure_values-1);
			switch(self_var.type){
			case OS_VALUE_TYPE_NULL:
				os->pushString(os->core->strings->typeof_null);
				return 1;

			case OS_VALUE_TYPE_BOOL:
				os->pushString(self_var.v.boolean ? os->core->strings->syntax_true : os->core->strings->syntax_false);
				return 1;

			case OS_VALUE_TYPE_NUMBER:
			case OS_VALUE_TYPE_STRING:
				os->core->pushValue(self_var);
				return 1;
			}
			Core::GCValue * self = self_var.getGCValue();
			if(!self){
				return 0;
			}
			switch(self->type){
			case OS_VALUE_TYPE_USERDATA:
			case OS_VALUE_TYPE_USERPTR:
				{
					Core::StringBuffer str(os);
					str += OS_TEXT("<");
					str += os->core->strings->typeof_userdata;
					str += OS_TEXT(":");
					str += Core::String(os, (OS_INT)self->value_id);
					str += OS_TEXT(">");
					os->pushString(str);
					return 1;
				}

			case OS_VALUE_TYPE_FUNCTION:
			case OS_VALUE_TYPE_CFUNCTION:
				{
					Core::StringBuffer str(os);
					str += OS_TEXT("<");
					str += os->core->strings->typeof_function;
					str += OS_TEXT(":");
					str += Core::String(os, (OS_INT)self->value_id);
					str += OS_TEXT(">");
					os->pushString(str);
					return 1;
				}
			case OS_VALUE_TYPE_ARRAY:
				{
					OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(self));
					Core::GCArrayValue * arr = (Core::GCArrayValue*)self;
					Core::StringBuffer buf(os);
					buf += OS_TEXT("[");
					Core::Value temp;
					for(int i = 0; i < arr->values.count; i++){
						if(i > 0){
							buf += OS_TEXT(",");
						}
						Core::Value value = arr->values[i];
						if(os->core->getPropertyValue(temp, os->core->check_recursion, value, false)){
							buf += OS_TEXT("<<RECURSION>>");
							continue;
						}
						Core::GCValue * gcvalue = value.getGCValue();
						if(gcvalue && gcvalue->table && gcvalue->table->count){
							os->core->setPropertyValue(os->core->check_recursion, value, Core::Value(true), false, false);
						}
						Core::String value_str = os->core->valueToString(value, true);
						if(value.type == OS_VALUE_TYPE_STRING){
							appendQuotedString(buf, value_str);
						}else{
							buf += value_str;
						}
					}
					buf += OS_TEXT("]");
					os->pushString(buf);
					return 1;
				}

			case OS_VALUE_TYPE_OBJECT:
				if(!self->table || !self->table->count){
					os->pushString(OS_TEXT("{}"));
					return 1;
				}
				{
					Core::StringBuffer buf(os);
					buf += OS_TEXT("{");
					int need_index = 0;
					Core::Property * prop = self->table->first;
					Core::Value temp;
					for(int i = 0; prop; prop = prop->next, i++){
						if(i > 0){
							buf += OS_TEXT(",");
						}
						if(prop->index.type == OS_VALUE_TYPE_NUMBER){
							if(prop->index.v.number != (OS_FLOAT)need_index){
								buf += String(os, prop->index.v.number, OS_AUTO_PRECISION);
								buf += OS_TEXT(":");
							}
							need_index = (int)(prop->index.v.number + 1);
						}else if(prop->index.type == OS_VALUE_TYPE_STRING){
							OS_ASSERT(!prop->index.v.string->table);
							appendQuotedString(buf, os->core->valueToString(prop->index));
							buf += OS_TEXT(":");
						}else{
							Core::GCValue * gcvalue = prop->index.getGCValue();
							if(os->core->getPropertyValue(temp, os->core->check_recursion, prop->index, false)){
								buf += OS_TEXT("<<RECURSION>>");
							}else{
								if(gcvalue && gcvalue->table && gcvalue->table->count){
									os->core->setPropertyValue(os->core->check_recursion, prop->index, Core::Value(true), false, false);
								}
								buf += os->core->valueToString(prop->index, true);
							}
							buf += OS_TEXT(":");
						}

						if(os->core->getPropertyValue(temp, os->core->check_recursion, prop->value, false)){
							buf += OS_TEXT("<<RECURSION>>");
							continue;
						}
						Core::GCValue * gcvalue = prop->value.getGCValue();
						if(gcvalue && gcvalue->table && gcvalue->table->count){
							os->core->setPropertyValue(os->core->check_recursion, prop->value, Core::Value(true), false, false);
						}

						Core::String value_str = os->core->valueToString(prop->value, true);
						if(prop->value.type == OS_VALUE_TYPE_STRING){
							appendQuotedString(buf, value_str);
						}else{
							buf += value_str;
						}
					}
					os->pushString(buf += OS_TEXT("}"));
					return 1;
				}
			}
			return 0;
		}

		static int push(OS * os, int params, int, int, void*)
		{
			Core::Value self_var = os->core->getStackValue(-params-1);
			Core::Value value = os->core->getStackValue(-params);
			OS_INT num_index = 0;
			switch(self_var.type){
			case OS_VALUE_TYPE_ARRAY:
				OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(self_var.v.arr));
				os->vectorAddItem(self_var.v.arr->values, value OS_DBG_FILEPOS);
				// os->pushNumber(self_var.v.arr->values.count);
				os->core->pushValue(value);
				return 1;

			case OS_VALUE_TYPE_OBJECT:
			case OS_VALUE_TYPE_USERDATA:
			case OS_VALUE_TYPE_USERPTR:
			case OS_VALUE_TYPE_FUNCTION:
			case OS_VALUE_TYPE_CFUNCTION:
				num_index = self_var.v.object->table ? self_var.v.object->table->next_index : 0;
				break;

			default:
				return 0;
			}
			os->core->setPropertyValue(self_var, Core::PropertyIndex(num_index), value, false, false);
			// os->pushNumber(self_var.v.object->table->count);
			os->core->pushValue(value);
			return 1;
		}

		static int pop(OS * os, int params, int, int, void*)
		{
			Core::Value self_var = os->core->getStackValue(-params-1);
			switch(self_var.type){
			case OS_VALUE_TYPE_ARRAY:
				OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(self_var.v.arr));
				if(self_var.v.arr->values.count > 0){
					os->core->pushValue(self_var.v.arr->values.lastElement());
					os->vectorRemoveAtIndex(self_var.v.arr->values, self_var.v.arr->values.count-1);
					return 1;
				}
				return 0;

			case OS_VALUE_TYPE_OBJECT:
			case OS_VALUE_TYPE_USERDATA:
			case OS_VALUE_TYPE_USERPTR:
			case OS_VALUE_TYPE_FUNCTION:
			case OS_VALUE_TYPE_CFUNCTION:
				if(self_var.v.object->table && self_var.v.object->table->count > 0){
					os->core->pushValue(self_var.v.object->table->last->value);
					Core::PropertyIndex index = *self_var.v.object->table->last;
					os->core->deleteValueProperty(self_var.v.object, index, false, false, false);
					return 1;
				}
				break;
			}
			return 0;
		}

		static int hasOwnProperty(OS * os, int params, int, int, void*)
		{
			Core::Value self_var = os->core->getStackValue(-params-1);
			Core::Value index = os->core->getStackValue(-params);
			Core::GCValue * self = self_var.getGCValue();
			if(self){
				os->pushBool( os->core->hasProperty(self, index, true, true, false) );
				return 1;
			}
			return 0;
		}

		static int hasProperty(OS * os, int params, int, int, void*)
		{
			Core::Value self_var = os->core->getStackValue(-params-1);
			Core::Value index = os->core->getStackValue(-params);
			Core::GCValue * self = self_var.getGCValue();
			if(self){
				os->pushBool( os->core->hasProperty(self, index, true, true, true) );
				return 1;
			}
			return 0;
		}

		static int sub(OS * os, int params, int, int, void*)
		{
			int start, len, size;
			Core::Value self_var = os->core->getStackValue(-params-1);
			switch(self_var.type){
			case OS_VALUE_TYPE_OBJECT:
				OS_ASSERT(dynamic_cast<Core::GCObjectValue*>(self_var.v.object));
				size = self_var.v.object->table ? self_var.v.object->table->count : 0;
				break;

			default:
				return 0;
			}
			switch(params){
			case 0:
				os->core->pushValue(self_var);
				return 1;

			case 1:
				start = os->toInt(-params);
				len = size;
				break;

			default:
				start = os->toInt(-params);
				len = os->toInt(-params+1);
			}
			if(start < 0){
				start = size + start;
				if(start < 0){
					start = 0;
				}
			}
			if(start >= size){
				os->newObject();
				return 1;
			}
			if(len < 0){
				len = size - start + len;
			}
			if(len <= 0){
				os->newObject();
				return 1;
			}
			if(start + len > size){
				len = size - start;
			}
			if(!start && len == size){
				os->core->pushValue(self_var);
				return 1;
			}
			OS_ASSERT(self_var.v.object->table && self_var.v.object->table->first);
			Core::GCObjectValue * object = os->core->pushObjectValue(self_var.v.object->prototype);
			Core::Property * prop = self_var.v.object->table->first;
			int i = 0;
			for(; i < start; i++){
				prop = prop->next;
				OS_ASSERT(prop);
			}
			Vector<Core::Value> captured_items;
			os->vectorReserveCapacity(captured_items, len*2 OS_DBG_FILEPOS);
			for(i = 0; i < len; i++, prop = prop->next){
				OS_ASSERT(prop);
				os->vectorAddItem(captured_items, prop->index OS_DBG_FILEPOS);
				os->vectorAddItem(captured_items, prop->value OS_DBG_FILEPOS);
			}
			for(i = 0; i < len; i++){
				os->core->setPropertyValue(object, captured_items[i*2], captured_items[i*2+1], false, false);
			}
			os->vectorClear(captured_items);
			return 1;
		}

		static int merge(OS * os, int params, int, int, void*)
		{
			if(params < 1) return 0;
			int offs = os->getAbsoluteOffs(-params);
			bool is_array = os->isArray(offs-1);
			if(is_array || os->isObject(offs-1)){
				for(int i = 0; i < params; i++){
					Core::Value value = os->core->getStackValue(offs+i);
					switch(value.type){
					case OS_VALUE_TYPE_ARRAY:
						{
							OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(value.v.arr));
							for(int j = 0; j < value.v.arr->values.count; j++){
								os->pushStackValue(offs-1);
								os->core->pushValue(value.v.arr->values[j]);
								os->addProperty();
							}
							break;
						}

					case OS_VALUE_TYPE_OBJECT:
						{
							OS_ASSERT(dynamic_cast<Core::GCObjectValue*>(value.v.object));
							if(value.v.object->table){
								Core::Property * prop = value.v.object->table->first;
								for(; prop; prop = prop->next){
									os->pushStackValue(offs-1);
									if(is_array){
										os->core->pushValue(prop->value);
										os->addProperty();
									}else{
										os->core->pushValue(prop->index);
										os->core->pushValue(prop->value);
										os->setProperty();
									}
								}
							}
							break;
						}
					}
				}
				os->pushStackValue(offs-1);
				return 1;
			}
			return 0;
		}

		static int getKeys(OS * os, int params, int, int, void*)
		{
			Core::Value value = os->core->getStackValue(-params-1);
			switch(value.type){
			case OS_VALUE_TYPE_ARRAY:
				{
					Core::GCArrayValue * arr = os->core->pushArrayValue(value.v.arr->values.count);
					for(int i = 0; i < value.v.arr->values.count; i++){
						os->vectorAddItem(arr->values, Core::Value(i) OS_DBG_FILEPOS);
					}
					return 1;
				}

			case OS_VALUE_TYPE_OBJECT:
				{
					if(value.v.object->table){
						Core::GCArrayValue * arr = os->core->pushArrayValue(value.v.object->table->count);
						Core::Property * prop = value.v.object->table->first;
						for(int i = 0; prop; prop = prop->next, i++){
							os->vectorAddItem(arr->values, prop->index OS_DBG_FILEPOS);
						}
					}else{
						os->newArray();
					}
					return 1;
				}
			}
			return 0;
		}

		static int getValues(OS * os, int params, int, int, void*)
		{
			Core::Value value = os->core->getStackValue(-params-1);
			switch(value.type){
			case OS_VALUE_TYPE_ARRAY:
				os->core->pushValue(value);
				return 1;

			case OS_VALUE_TYPE_OBJECT:
				{
					if(value.v.object->table){
						Core::GCArrayValue * arr = os->core->pushArrayValue(value.v.object->table->count);
						Core::Property * prop = value.v.object->table->first;
						for(int i = 0; prop; prop = prop->next, i++){
							os->vectorAddItem(arr->values, prop->value OS_DBG_FILEPOS);
						}
					}else{
						os->newArray();
					}
					return 1;
				}
			}
			return 0;
		}
	};
	FuncDef list[] = {
		{OS_TEXT("rawget"), Object::rawget},
		{OS_TEXT("rawset"), Object::rawset},
		{OS_TEXT("__get@osValueId"), Object::getValueId},
		{core->strings->__len, Object::length},
		// {OS_TEXT("__get@length"), Object::length},
		{core->strings->__iter, Object::iterator},
		{OS_TEXT("reverseIter"), Object::reverseIterator},
		{core->strings->__valueof, Object::valueof},
		{OS_TEXT("sort"), Object::sort},
		{OS_TEXT("rsort"), Object::rsort},
		{OS_TEXT("ksort"), Object::ksort},
		{OS_TEXT("krsort"), Object::krsort},
		{OS_TEXT("push"), Object::push},
		{OS_TEXT("pop"), Object::pop},
		{OS_TEXT("hasOwnProperty"), Object::hasOwnProperty},
		{OS_TEXT("hasProperty"), Object::hasProperty},
		{OS_TEXT("merge"), Object::merge},
		{OS_TEXT("getKeys"), Object::getKeys},
		{OS_TEXT("getValues"), Object::getValues},
		{OS_TEXT("__get@keys"), Object::getKeys},
		{OS_TEXT("__get@values"), Object::getValues},
		{}
	};
	core->pushValue(core->prototypes[Core::PROTOTYPE_OBJECT]);
	setFuncs(list);
	pop();
}

void OS::initArrayClass()
{
	struct Array
	{
		static int sub(OS * os, int params, int, int, void*)
		{
			int start, len, size;
			Core::Value self_var = os->core->getStackValue(-params-1);
			switch(self_var.type){
			case OS_VALUE_TYPE_ARRAY:
				OS_ASSERT(dynamic_cast<Core::GCArrayValue*>(self_var.v.arr));
				size = self_var.v.arr->values.count;
				break;

			default:
				return 0;
			}
			switch(params){
			case 0:
				os->core->pushValue(self_var);
				return 1;

			case 1:
				start = os->toInt(-params);
				len = size;
				break;

			default:
				start = os->toInt(-params);
				len = os->toInt(-params+1);
			}
			if(start < 0){
				start = size + start;
				if(start < 0){
					start = 0;
				}
			}
			if(start >= size){
				os->newArray();
				return 1;
			}
			if(len < 0){
				len = size - start + len;
			}
			if(len <= 0){
				os->newArray();
				return 1;
			}
			if(start + len > size){
				len = size - start;
			}
			if(!start && len == size){
				os->core->pushValue(self_var);
				return 1;
			}
			Core::GCArrayValue * arr = os->core->pushArrayValue(len);
			for(int i = 0; i < len; i++){
				os->vectorAddItem(arr->values, self_var.v.arr->values[start+i] OS_DBG_FILEPOS);
			}
			return 1;
		}
	};
	FuncDef list[] = {
		{OS_TEXT("sub"), Array::sub},
		{}
	};
	core->pushValue(core->prototypes[Core::PROTOTYPE_ARRAY]);
	setFuncs(list);
	pop();
}

void OS::initStringClass()
{
	struct String
	{
		static int length(OS * os, int params, int, int, void*)
		{
			Core::Value self_var = os->core->getStackValue(-params-1);
			Core::GCValue * self = self_var.getGCValue();
			if(self){
				if(self->type == OS_VALUE_TYPE_STRING){
					Core::GCStringValue * string = (Core::GCStringValue*)self;
					os->pushNumber(string->getLen());
					// os->pushNumber(os->core->valueToString(self).getDataSize() / sizeof(OS_CHAR));
					return 1;
				}
				os->core->pushOpResultValue(Core::Program::OP_LENGTH, self_var);
				return 1;
			}
			return 0;
		}

		static int sub(OS * os, int params, int, int, void*)
		{
			int start, len;
			OS::String str = os->toString(-params-1);
			int size = str.getLen();
			switch(params){
			case 0:
				os->pushStackValue(-params-1);
				return 1;

			case 1:
				start = os->toInt(-params);
				len = size;
				break;

			default:
				start = os->toInt(-params);
				len = os->toInt(-params+1);
			}
			if(start < 0){
				start = size + start;
				if(start < 0){
					start = 0;
				}
			}
			if(start >= size){
				os->pushString(OS_TEXT(""));
				return 1;
			}
			if(len < 0){
				len = size - start + len;
			}
			if(len <= 0){
				os->pushString(OS_TEXT(""));
				return 1;
			}
			if(start + len > size){
				len = size - start;
			}
			if(!start && len == size){
				os->pushStackValue(-params-1);
				return 1;
			}
			os->pushString(str.toChar() + start, len);
			return 1;
		}
	};
	FuncDef list[] = {
		{core->strings->__len, String::length},
		{OS_TEXT("sub"), String::sub},
		// {OS_TEXT("__get@length"), String::length},
		{}
	};
	core->pushValue(core->prototypes[Core::PROTOTYPE_STRING]);
	setFuncs(list);
	pop();
}

void OS::initFunctionClass()
{
	struct Function
	{
		static int apply(OS * os, int params, int, int need_ret_values, void*)
		{
			int offs = os->getAbsoluteOffs(-params);
			os->pushStackValue(offs-1); // self as func
			if(params < 1){
				os->pushNull();
				return os->call(0, need_ret_values);
			}
			os->pushStackValue(offs); // first param - new this

			Core::Value array_var = os->core->getStackValue(offs+1);
			if(array_var.type == OS_VALUE_TYPE_ARRAY){
				int count = array_var.v.arr->values.count;
				for(int i = 0; i < count; i++){
					os->core->pushValue(array_var.v.arr->values[i]);
				}
				return os->call(count, need_ret_values);
			}
			return os->call(0, need_ret_values);
		}

		static int call(OS * os, int params, int, int need_ret_values, void*)
		{
#if 1 // speed optimization
			return os->call(params-1, need_ret_values);
#else
			int offs = os->getAbsoluteOffs(-params);
			os->pushStackValue(offs-1); // self as func
			if(params < 1){
				os->pushNull(); // this
				return os->call(0, need_ret_values);
			}
			os->pushStackValue(offs); // first param - new this
			for(int i = 1; i < params; i++){
				os->pushStackValue(offs + i);
			}
			return os->call(params-1, need_ret_values);
#endif
		}

		static int applyEnv(OS * os, int params, int, int need_ret_values, void *)
		{
			Core::Value save_env;
			Core::Value func = os->core->getStackValue(-params-1);
			if(func.type == OS_VALUE_TYPE_FUNCTION){
				save_env = func.v.func->env;
				func.v.func->env = os->core->getStackValue(-params).getGCValue();
			}
			os->remove(-params);
			int r = apply(os, params-1, 0, need_ret_values, NULL);
			if(func.type == OS_VALUE_TYPE_FUNCTION){
				func.v.func->env = save_env;
			}
			return r;
		}

		static int callEnv(OS * os, int params, int, int need_ret_values, void *)
		{
			Core::Value save_env;
			Core::Value func = os->core->getStackValue(-params-1);
			if(func.type == OS_VALUE_TYPE_FUNCTION){
				save_env = func.v.func->env;
				func.v.func->env = os->core->getStackValue(-params).getGCValue();
			}
			os->remove(-params);
			int r = call(os, params-1, 0, need_ret_values, NULL);
			if(func.type == OS_VALUE_TYPE_FUNCTION){
				func.v.func->env = save_env;
			}
			return r;
		}

		static int getEnv(OS * os, int params, int, int, void*)
		{
			Core::Value func = os->core->getStackValue(-params-1);
			if(func.type == OS_VALUE_TYPE_FUNCTION){
				os->core->pushValue(func.v.func->env);
				return 1;
			}
			return 0;
		}

		static int setEnv(OS * os, int params, int, int, void*)
		{
			Core::Value func = os->core->getStackValue(-params-1);
			if(func.type == OS_VALUE_TYPE_FUNCTION){
				Core::Value env = os->core->getStackValue(-params);
				func.v.func->env = env.getGCValue();
			}
			return 0;
		}

		static int iterator(OS * os, int params, int, int need_ret_values, void*)
		{
			os->pushStackValue(-params-1); // self as func
			return 1;
		}
	};
	FuncDef list[] = {
		{OS_TEXT("apply"), Function::apply},
		{OS_TEXT("applyEnv"), Function::applyEnv},
		{OS_TEXT("call"), Function::call},
		{OS_TEXT("callEnv"), Function::callEnv},
		{OS_TEXT("__get@") OS_ENV_VAR_NAME, Function::getEnv},
		{OS_TEXT("__set@") OS_ENV_VAR_NAME, Function::setEnv},
		{core->strings->__iter, Function::iterator},
		{}
	};
	core->pushValue(core->prototypes[Core::PROTOTYPE_FUNCTION]);
	setFuncs(list);
	pop();
}

/*
The following functions are based on a C++ class MTRand by
Richard J. Wagner. For more information see the web page at
http://www-personal.engin.umich.edu/~wagnerr/MersenneTwister.html

It's port from PHP framework.
*/

#define OS_RAND_N             RAND_STATE_SIZE      /* length of state vector */
#define OS_RAND_M             (397)                /* a period parameter */
#define OS_RAND_hiBit(u)      ((u) & 0x80000000U)  /* mask all but highest   bit of u */
#define OS_RAND_loBit(u)      ((u) & 0x00000001U)  /* mask all but lowest    bit of u */
#define OS_RAND_loBits(u)     ((u) & 0x7FFFFFFFU)  /* mask     the highest   bit of u */
#define OS_RAND_mixBits(u, v) (OS_RAND_hiBit(u)|OS_RAND_loBits(v)) /* move hi bit of u to hi bit of v */

#define OS_RAND_twist(m,u,v)  (m ^ (OS_RAND_mixBits(u,v)>>1) ^ ((OS_U32)(-(OS_INT32)(OS_RAND_loBit(u))) & 0x9908b0dfU))
#define OS_RAND_MAX 0x7FFFFFFF		/* (1<<31) - 1 */ 

#define OS_RAND_RANGE(__n, __min, __max, __tmax) \
	(__n) = (__min) + (long) ((double) ( (double) (__max) - (__min) + 1.0) * ((__n) / ((__tmax) + 1.0)))

#if defined _MSC_VER && !defined IW_SDK
#include <windows.h>
#define OS_RAND_GENERATE_SEED() (((long) (time(0) * GetCurrentProcessId())) ^ ((long) (1000000.0)))
// #elif !defined IW_SDK
// #define OS_RAND_GENERATE_SEED() (((long) (time(0) * getpid())) ^ ((long) (1000000.0)))
#else
#define OS_RAND_GENERATE_SEED() (((long) (time(0))) ^ ((long) (1000000.0)))
#endif 

void OS::Core::randInitialize(OS_U32 seed)
{
	rand_seed = seed;

	OS_U32 * s = rand_state;
	OS_U32 * r = s;

	*s++ = seed & 0xffffffffU;
	for(int i = 1; i < OS_RAND_N; ++i ) {
		*s++ = ( 1812433253U * ( *r ^ (*r >> 30) ) + i ) & 0xffffffffU;
		r++;
	}

	randReload();
}

void OS::Core::randReload()
{
	/* Generate N new values in state
	Made clearer and faster by Matthew Bellew (matthew.bellew@home.com) */

	OS_U32 * state = rand_state;
	OS_U32 * p = state;
	int i;

	for(i = OS_RAND_N - OS_RAND_M; i--; ++p){
		*p = OS_RAND_twist(p[OS_RAND_M], p[0], p[1]);
	}
	for(i = OS_RAND_M; --i; ++p){
		*p = OS_RAND_twist(p[OS_RAND_M-OS_RAND_N], p[0], p[1]);
	}
	*p = OS_RAND_twist(p[OS_RAND_M-OS_RAND_N], p[0], state[0]);
	rand_left = OS_RAND_N;
	rand_next = state;
}

double OS::Core::getRand()
{
	/* Pull a 32-bit integer from the generator state
	Every other access function simply transforms the numbers extracted here */

	if(!rand_left){
		if(!rand_next){
			randInitialize(OS_RAND_GENERATE_SEED());
		}else{
			randReload();
		}
	}
	--rand_left;

	OS_U32 s1 = *rand_next++;
	s1 ^= (s1 >> 11);
	s1 ^= (s1 <<  7) & 0x9d2c5680U;
	s1 ^= (s1 << 15) & 0xefc60000U;
	return (double)((s1 ^ (s1 >> 18))>>1) / (double)OS_RAND_MAX;
}

double OS::Core::getRand(double up)
{
	return ::floor(getRand()*(up-1) + 0.5f);
}

double OS::Core::getRand(double min, double max)
{
	return getRand() * (max - min) + min;
}

#define OS_MATH_PI ((OS_NUMBER)3.1415926535897932384626433832795)
#define OS_RADIANS_PER_DEGREE (OS_MATH_PI/180.0f)

void OS::initMathModule()
{
	struct Math
	{
		static int minmax(OS * os, int params, OS_EOpcode opcode)
		{
			OS_ASSERT(params >= 0);
			if(params <= 1){
				return params;
			}
			int params_offs = os->getAbsoluteOffs(-params);
			os->pushStackValue(params_offs); // save temp result
			for(int i = 1; i < params; i++){
				os->pushStackValue(-1); // copy temp result
				os->pushStackValue(params_offs + i);
				os->runOp(opcode); // remove params & push op result
				if(!os->toBool()){
					os->pop(2); // remove op result and temp result
					os->pushStackValue(params_offs + i); // save temp result
					continue;
				}
				os->pop();
			}
			return 1;
		}

		static int min_func(OS * os, int params, int, int, void*)
		{
			return minmax(os, params, OP_LOGIC_LE);
		}

		static int max_func(OS * os, int params, int, int, void*)
		{
			return minmax(os, params, OP_LOGIC_GE);
		}

		static double abs(double p)
		{
			return ::fabs(p);
		}

		static double ceil(double p)
		{
			return ::ceil(p);
		}

		static double floor(double p)
		{
			return ::floor(p);
		}

		static double round(double a, int precision)
		{
			if(precision <= 0){
				if(precision < 0){
					double p = 10.0f;
					for(int i = -precision-1; i > 0; i--){
						p *= 10.0f;
					}
					return ::floor(a / p + 0.5f) * p;
				}
				return ::floor(a + 0.5f);
			}
			double p = 10.0f;
			for(int i = precision-1; i > 0; i--){
				p *= 10.0f;
			}
			return ::floor(a * p + 0.5f) / p;
		}

		static double sin(double p)
		{
			return ::sin(p);
		}

		static double sinh(double p)
		{
			return ::sinh(p);
		}

		static double cos(double p)
		{
			return ::cos(p);
		}

		static double cosh(double p)
		{
			return ::cosh(p);
		}

		static double tan(double p)
		{
			return ::tan(p);
		}

		static double tanh(double p)
		{
			return ::tanh(p);
		}

		static double acos(double p)
		{
			return ::acos(p);
		}

		static double asin(double p)
		{
			return ::asin(p);
		}

		static double atan(double p)
		{
			return ::atan(p);
		}

		static double atan2(double y, double x)
		{
			return ::atan2(y, x);
		}

		static double exp(double p)
		{
			return ::exp(p);
		}

		static int frexp(OS * os, int params, int, int, void*)
		{
			if(!params) return 0;
			int e;
			os->pushNumber(::frexp(os->toNumber(-params), &e));
			os->pushNumber(e);
			return 2;
		}

		static double ldexp(double x, int y)
		{
			return ::ldexp(x, y);
		}

		static double pow(double x, double y)
		{
			return ::pow(x, y);
		}

		static int random(OS * os, int params, int, int, void*)
		{
			OS::Core * core = os->core;
			switch(params){
			case 0:
				os->pushNumber(core->getRand());
				return 1;

			case 1:
				os->pushNumber(core->getRand(os->toNumber(-params)));
				return 1;

			case 2:
			default:
				os->pushNumber(core->getRand(os->toNumber(-params), os->toNumber(-params+1)));
				return 1;
			}
			return 0;
		}

		static int getrandseed(OS * os, int params, int, int, void*)
		{
			os->pushNumber((OS_NUMBER)os->core->rand_seed);
			return 1;
		}

		static int setrandseed(OS * os, int params, int, int, void*)
		{
			if(!params) return 0;
			os->core->rand_seed = (OS_U32)os->toNumber(-params);
			return 0;
		}

		static double fmod(double x, double y)
		{
			return ::fmod(x, y);
		}

		static int modf(OS * os, int params, int, int, void*)
		{
			if(!params) return 0;
			double ip;
			double fp = ::modf(os->toNumber(-params), &ip);
			os->pushNumber(ip);
			os->pushNumber(fp);
			return 2;
		}

		static double sqrt(double p)
		{
			return ::sqrt(p);
		}

		static int log(OS * os, int params, int, int, void*)
		{
			if(!params) return 0;
			double x = os->toNumber(-params);
			OS_NUMBER base;
			if(params > 1 && os->isNumber(-params+1, &base)){
				if(base == 10){
					os->pushNumber(::log10(x));
				}else{
					os->pushNumber(::log(x)/::log(base));
				}
			}else{
				os->pushNumber(::log(x));
			}
			return 1;
		}

		static double deg(double p)
		{
			return p / OS_RADIANS_PER_DEGREE;
		}
		
		static double rad(double p)
		{
			return p * OS_RADIANS_PER_DEGREE;
		}
	};
	FuncDef list[] = {
		{OS_TEXT("min"), Math::min_func},
		{OS_TEXT("max"), Math::max_func},
		def(OS_TEXT("abs"), Math::abs),
		def(OS_TEXT("ceil"), Math::ceil),
		def(OS_TEXT("floor"), Math::floor),
		def(OS_TEXT("round"), Math::round),
		def(OS_TEXT("sin"), Math::sin),
		def(OS_TEXT("sinh"), Math::sinh),
		def(OS_TEXT("cos"), Math::cos),
		def(OS_TEXT("cosh"), Math::cosh),
		def(OS_TEXT("tan"), Math::tan),
		def(OS_TEXT("tanh"), Math::tanh),
		def(OS_TEXT("acos"), Math::acos),
		def(OS_TEXT("asin"), Math::asin),
		def(OS_TEXT("atan"), Math::atan),
		def(OS_TEXT("atan2"), Math::atan2),
		def(OS_TEXT("exp"), Math::exp),
		{OS_TEXT("frexp"), Math::frexp},
		def(OS_TEXT("ldexp"), Math::ldexp),
		def(OS_TEXT("pow"), Math::pow),
		{OS_TEXT("random"), Math::random},
		{OS_TEXT("__get@randseed"), Math::getrandseed},
		{OS_TEXT("__set@randseed"), Math::setrandseed},
		def(OS_TEXT("fmod"), Math::fmod),
		{OS_TEXT("modf"), Math::modf},
		def(OS_TEXT("sqrt"), Math::sqrt),
		{OS_TEXT("log"), Math::log},
		def(OS_TEXT("deg"), Math::deg),
		def(OS_TEXT("rad"), Math::rad),
		{}
	};
	NumberDef numbers[] = {
		{OS_TEXT("PI"), OS_MATH_PI},
		{OS_TEXT("MAX_NUMBER"), OS_MAX_NUMBER},
		{}
	};

	getModule(OS_TEXT("math"));
	setFuncs(list);
	setNumbers(numbers);
	pop();
}

void OS::initGCModule()
{
	struct GC
	{
		static int getAllocatedBytes(OS * os, int params, int, int, void*)
		{
			os->pushNumber(os->getAllocatedBytes());
			return 1;
		}
		static int getMaxAllocatedBytes(OS * os, int params, int, int, void*)
		{
			os->pushNumber(os->getMaxAllocatedBytes());
			return 1;
		}
		static int getCachedBytes(OS * os, int params, int, int, void*)
		{
			os->pushNumber(os->getCachedBytes());
			return 1;
		}
		static int getNumObjects(OS * os, int params, int, int, void*)
		{
			os->pushNumber(os->core->values.count);
			return 1;
		}
		static int getNumCreatedObjects(OS * os, int params, int, int, void*)
		{
			os->pushNumber(os->core->num_created_values);
			return 1;
		}
		static int getNumDestroyedObjects(OS * os, int params, int, int, void*)
		{
			os->pushNumber(os->core->num_destroyed_values);
			return 1;
		}
	};
	FuncDef list[] = {
		{OS_TEXT("__get@allocatedBytes"), GC::getAllocatedBytes},
		{OS_TEXT("__get@maxAllocatedBytes"), GC::getMaxAllocatedBytes},
		{OS_TEXT("__get@cachedBytes"), GC::getCachedBytes},
		{OS_TEXT("__get@numObjects"), GC::getNumObjects},
		{OS_TEXT("__get@numCreatedObjects"), GC::getNumCreatedObjects},
		{OS_TEXT("__get@numDestroyedObjects"), GC::getNumDestroyedObjects},
		{}
	};

	getModule(OS_TEXT("GC"));
	setFuncs(list);
	pop();
}

void OS::initLangTokenizerModule()
{
	struct LangTokenizer
	{
		enum {
			TOKEN_TYPE_STRING,
			TOKEN_TYPE_NUMBER,
			TOKEN_TYPE_NAME,
			TOKEN_TYPE_OPERATOR
		};

		static int getTokenType(Core::TokenType type)
		{
			switch(type){
			case Core::Tokenizer::NAME:
				return TOKEN_TYPE_NAME;

			case Core::Tokenizer::STRING:
				return TOKEN_TYPE_STRING;

			case Core::Tokenizer::NUMBER:
				return TOKEN_TYPE_NUMBER;
			}
			return TOKEN_TYPE_OPERATOR;
		}

		static void pushTokensAsObject(OS * os, Core::Tokenizer& tokenizer)
		{
			os->newArray();
			int count = tokenizer.getNumTokens();
			for(int i = 0; i < count; i++){
				os->pushStackValue(-1);
				os->newObject();
				{
					Core::TokenData * token = tokenizer.getToken(i);

					os->pushStackValue(-1);
					os->pushString(OS_TEXT("str"));
					os->pushString(token->str);
					os->setProperty();
#if 0
					os->pushStackValue(-1);
					os->pushString(OS_TEXT("line"));
					os->pushNumber(token->line+1);
					os->setProperty();

					os->pushStackValue(-1);
					os->pushString(OS_TEXT("pos"));
					os->pushNumber(token->pos+1);
					os->setProperty();
#endif
					os->pushStackValue(-1);
					os->pushString(OS_TEXT("type"));
					os->pushNumber(getTokenType(token->type));
					os->setProperty();
				}
				os->addProperty();
			}
		}

		static int parseText(OS * os, int params, int, int, void*)
		{
			String str = os->toString(-params);
			if(str.getDataSize() == 0){
				return 0;
			}
			Core::Tokenizer tokenizer(os);
			tokenizer.parseText(str.toChar(), str.getLen(), String(os));
			pushTokensAsObject(os, tokenizer);
			return 1;
		}

		static int parseFile(OS * os, int params, int, int, void*)
		{
			String filename = os->resolvePath(os->toString(-params));
			if(filename.getDataSize() == 0){
				return 0;
			}
			Core::FileStreamReader file(os, filename);
			if(!file.f){
				return 0;
			}
			Core::MemStreamWriter file_data(os);
			file_data.writeFromStream(&file);

			Core::Tokenizer tokenizer(os);
			tokenizer.parseText((OS_CHAR*)file_data.buffer.buf, file_data.buffer.count, filename);

			pushTokensAsObject(os, tokenizer);
			return 1;
		}
	};

	FuncDef list[] = {
		{OS_TEXT("parseText"), LangTokenizer::parseText},
		{OS_TEXT("parseFile"), LangTokenizer::parseFile},
		{}
	};

	NumberDef numbers[] = {
		{OS_TEXT("TOKEN_TYPE_STRING"), LangTokenizer::TOKEN_TYPE_STRING},
		{OS_TEXT("TOKEN_TYPE_NUMBER"), LangTokenizer::TOKEN_TYPE_NUMBER},
		{OS_TEXT("TOKEN_TYPE_NAME"), LangTokenizer::TOKEN_TYPE_NAME},
		{OS_TEXT("TOKEN_TYPE_OPERATOR"), LangTokenizer::TOKEN_TYPE_OPERATOR},
		{}
	};

	getModule(OS_TEXT("LangTokenizer"));
	setFuncs(list);
	setNumbers(numbers);
	pop();
}

#define OS_AUTO_TEXT(exp) OS_TEXT(#exp)

void OS::initPreScript()
{
	eval(OS_AUTO_TEXT(
		// it's ObjectScript code here
		Object.__get@length = function(){ return #this }

	modules_loaded = {}
	function require(filename, required){
		filename = resolvePath(filename)
			return filename && (modules_loaded.rawget(filename) 
			|| function(){
				modules_loaded[filename] = {} // block recursive require
				modules_loaded[filename] = compileFile(filename, required)()
					return modules_loaded[filename]
		}())
	}
	));
}

void OS::initPostScript()
{
	eval(OS_AUTO_TEXT(
		// it's ObjectScript code here
		Object.__setempty = Object.push
		Object.__getempty = Object.pop
		));
}

int OS::Core::syncRetValues(int need_ret_values, int cur_ret_values)
{
	if(cur_ret_values > need_ret_values){
		pop(cur_ret_values - need_ret_values);
	}else{ 
		for(; cur_ret_values < need_ret_values; cur_ret_values++){
			pushNull();
		}
	}
	return need_ret_values;
}

OS::Core::GCObjectValue * OS::Core::initObjectInstance(GCObjectValue * object)
{
	struct Lib {
		static GCObjectValue * initObjectInstance_r(Core * core, GCObjectValue * object, GCValue * prototype)
		{
			if(prototype->prototype){
				initObjectInstance_r(core, object, prototype->prototype);
			}
			Value value;
			if(core->getPropertyValue(value, prototype, PropertyIndex(core->strings->__object, PropertyIndex::KeepStringIndex()), false)){
				GCValue * object_props = value.getGCValue();
				if(object_props && object_props->table){
					Property * prop = object_props->table->first;
					for(; prop; prop = prop->next){
						core->pushCloneValue(prop->value);
						core->setPropertyValue(object, *prop, core->stack_values.lastElement(), true, true);
						core->pop();
					}
				}
			}

			return object;
		}
	};
	if(object->prototype){
		Lib::initObjectInstance_r(this, object, object->prototype);
	}
	return object;
}

void OS::Core::pushArguments(StackFunction * stack_func)
{
	if(!stack_func->arguments){
		int i;
		GCArrayValue * args = pushArrayValue();
		Upvalues * func_upvalues = stack_func->locals;
		int num_params = stack_func->num_params - stack_func->num_extra_params;
		for(i = 0; i < num_params; i++){
			allocator->vectorAddItem(args->values, func_upvalues->locals[i] OS_DBG_FILEPOS);
		}
		int num_locals = func_upvalues->num_locals;
		for(i = 0; i < stack_func->num_extra_params; i++){
			allocator->vectorAddItem(args->values, func_upvalues->locals[i + num_locals] OS_DBG_FILEPOS);
		}
		stack_func->arguments = args;
	}else{
		pushValue(stack_func->arguments);
	}
}

void OS::Core::pushArgumentsWithNames(StackFunction * stack_func)
{
	int i;
	GCObjectValue * args = pushObjectValue();
	Upvalues * func_upvalues = stack_func->locals;
	FunctionDecl * func_decl = stack_func->func->func_decl;
	int num_params = stack_func->num_params - stack_func->num_extra_params;
	for(i = 0; i < num_params; i++){
		setPropertyValue(args, PropertyIndex(func_decl->locals[i].name.string, PropertyIndex::KeepStringIndex()), func_upvalues->locals[i], false, false);
	}
	int num_locals = func_upvalues->num_locals;
	if(num_params < func_decl->num_params){
		for(; i < func_decl->num_params; i++){
			setPropertyValue(args, PropertyIndex(func_decl->locals[i].name.string, PropertyIndex::KeepStringIndex()), Value(), false, false);
		}
	}else{
		for(i = 0; i < stack_func->num_extra_params; i++){
			setPropertyValue(args, Value(args->table ? args->table->next_index : 0), func_upvalues->locals[i + num_locals], false, false);
		}
	}
}

void OS::Core::pushRestArguments(StackFunction * stack_func)
{
	if(!stack_func->rest_arguments){
		GCArrayValue * args = pushArrayValue();
		Upvalues * func_upvalues = stack_func->locals;
		int num_locals = func_upvalues->num_locals;
		for(int i = 0; i < stack_func->num_extra_params; i++){
			allocator->vectorAddItem(args->values, func_upvalues->locals[i + num_locals] OS_DBG_FILEPOS);
		}
		stack_func->rest_arguments = args;
	}else{
		pushValue(stack_func->rest_arguments);
	}
}

void OS::Core::pushBackTrace(int skip_funcs, int max_trace_funcs)
{
	GCArrayValue * arr = pushArrayValue();

	String function_str(allocator, OS_TEXT("function"));
	String name_str(allocator, OS_TEXT("name"));
	String file_str(allocator, OS_TEXT("file"));
	String line_str(allocator, OS_TEXT("line"));
	String pos_str(allocator, OS_TEXT("pos"));
	String token_str(allocator, OS_TEXT("token"));
	String object_str(allocator, OS_TEXT("object"));
	String arguments_str(allocator, OS_TEXT("arguments"));
	String core_str(allocator, OS_TEXT("<<CORE>>"));
	String lambda_str(allocator, OS_TEXT("<<lambda>>"));

	for(int i = call_stack_funcs.count-1-skip_funcs; i >= 0 && arr->values.count < max_trace_funcs; i--){
		StackFunction * stack_func = call_stack_funcs.buf + i;

		Program * prog = stack_func->func->prog;
		if(!stack_func->func->name && !prog->filename.getDataSize()){
			continue;
		}

		GCObjectValue * obj = pushObjectValue();
		setPropertyValue(obj, PropertyIndex(name_str, PropertyIndex::KeepStringIndex()), stack_func->func->name ? stack_func->func->name : lambda_str.string, false, false);
		setPropertyValue(obj, PropertyIndex(function_str, PropertyIndex::KeepStringIndex()), stack_func->func, false, false);

		const String& filename = prog->filename.getDataSize() ? prog->filename : core_str;
		setPropertyValue(obj, PropertyIndex(file_str, PropertyIndex::KeepStringIndex()), filename.string, false, false);

		Program::DebugInfoItem * debug_info = NULL;
		if(prog->filename.getDataSize() && prog->debug_info.count > 0){
			int opcode_pos = stack_func->opcodes.getPos() + stack_func->func->func_decl->opcodes_pos;
			debug_info = prog->getDebugInfo(opcode_pos);
		}
		setPropertyValue(obj, PropertyIndex(line_str, PropertyIndex::KeepStringIndex()), debug_info ? debug_info->line : Value(), false, false);
		setPropertyValue(obj, PropertyIndex(pos_str, PropertyIndex::KeepStringIndex()), debug_info ? debug_info->pos : Value(), false, false);
		setPropertyValue(obj, PropertyIndex(token_str, PropertyIndex::KeepStringIndex()), debug_info ? debug_info->token.string : Value(), false, false);

		setPropertyValue(obj, PropertyIndex(object_str, PropertyIndex::KeepStringIndex()), stack_func->self, false, false);

		pushArgumentsWithNames(stack_func);
		setPropertyValue(obj, PropertyIndex(arguments_str, PropertyIndex::KeepStringIndex()), stack_values.lastElement(), false, false);
		pop(); // remove args

		setPropertyValue(arr, Value(arr->values.count), obj, false, false);
		pop(); // remove obj
	}
}

int OS::Core::call(int params, int ret_values, GCValue * self_for_proto, bool allow_only_enter_func)
{
	if(terminated){
		error(OS_E_ERROR, OS_TEXT("ObjectScript is terminated, you could reset terminate state using OS::resetTerminated if necessary"));
		pop(params + 2);
		return syncRetValues(ret_values, 0);
	}
	if(stack_values.count >= 2+params){
		int end_stack_size = stack_values.count-2-params;
		Value func_value = stack_values[stack_values.count-2-params];
		switch(func_value.type){
		case OS_VALUE_TYPE_FUNCTION:
			{
				Value self = stack_values[stack_values.count-1-params];
				enterFunction(func_value.v.func, self, self_for_proto, params, 2, ret_values);
				if(allow_only_enter_func){
					return 0;
				}
				ret_values = execute();
				OS_ASSERT(stack_values.count == end_stack_size + ret_values);
				return ret_values;
			}

		case OS_VALUE_TYPE_CFUNCTION:
			{
				int stack_size_without_params = getStackOffs(-2-params);
				GCCFunctionValue * cfunc_value = func_value.v.cfunc;
				if(cfunc_value->num_closure_values > 0){
					reserveStackValues(stack_values.count + cfunc_value->num_closure_values);
					Value * closure_values = (Value*)(cfunc_value + 1);
					OS_MEMCPY(stack_values.buf + stack_values.count, closure_values, sizeof(Value)*cfunc_value->num_closure_values);
					stack_values.count += cfunc_value->num_closure_values;
				}
				int func_ret_values = cfunc_value->func(allocator, params, cfunc_value->num_closure_values, ret_values, cfunc_value->user_param);
#if 0
				if(cfunc_value->num_closure_values > 0){
					Value * closure_values = (Value*)(cfunc_value + 1);
					OS_MEMCPY(closure_values, stack_values.buf + stack_values.count, sizeof(Value)*cfunc_value->num_closure_values);
				}
#endif
				int remove_values = getStackOffs(-func_ret_values) - stack_size_without_params;
				OS_ASSERT(remove_values >= 0);
				removeStackValues(stack_size_without_params, remove_values);
				ret_values = syncRetValues(ret_values, func_ret_values);
				OS_ASSERT(stack_values.count == end_stack_size + ret_values);
				return ret_values;
			}

		case OS_VALUE_TYPE_OBJECT:
			{
				GCValue * object = initObjectInstance(pushObjectValue(func_value.v.value));
				object->is_object_instance = true;

				bool prototype_enabled = true;
				Value func;
				if(getPropertyValue(func, func_value, PropertyIndex(strings->__construct, PropertyIndex::KeepStringIndex()), prototype_enabled)
					&& func.isFunction())
				{
					pushValue(func);
					pushValue(object);
					moveStackValues(-3, 3, -3-params);
					call(params, 1);

					// 4 values in stack here
					object = stack_values.lastElement().getGCValue();
					if(object){
						stack_values.count -= 3;
						stack_values.lastElement() = object;
						return syncRetValues(ret_values, 1);
					}else{
						pop(4);
						return syncRetValues(ret_values, 0);
					}
				}
				removeStackValues(-3, 2);
				return syncRetValues(ret_values, 1);
			}

		case OS_VALUE_TYPE_USERDATA:
		case OS_VALUE_TYPE_USERPTR:
			{
				bool prototype_enabled = true;
				Value func;
				if(getPropertyValue(func, func_value, PropertyIndex(strings->__construct, PropertyIndex::KeepStringIndex()), prototype_enabled)
					&& func.isFunction())
				{
					stack_values[stack_values.count-2-params] = func;
					stack_values[stack_values.count-1-params] = func_value;
					return call(params, ret_values); // request result
				}
				break;
			}
		}
	}
	// OS_ASSERT(false);
	pop(params + 2);
	return syncRetValues(ret_values, 0);
}

bool OS::compileFile(const String& p_filename, bool required)
{
	String filename = resolvePath(p_filename);
	String compiled_filename = getCompiledFilename(filename);
	bool sourcecode_file_exist = isFileExist(filename);
	bool compiled_file_exist = isFileExist(compiled_filename);
	if(compiled_file_exist && sourcecode_file_exist){
		if(core->settings.primary_compiled_file || checkFileUsage(filename, compiled_filename) == LOAD_COMPILED_FILE){
			sourcecode_file_exist = false;
		}else{
			compiled_file_exist = false;
		}
	}
	if(!sourcecode_file_exist && !compiled_file_exist){
		if(required){
			core->error(OS_E_ERROR, String::format(this, OS_TEXT("required filename %s is not exist"), p_filename.toChar()));
			return false;
		}
		core->error(OS_E_WARNING, String::format(this, OS_TEXT("filename %s is not exist"), p_filename.toChar()));
		return false;
	}
	if(!sourcecode_file_exist){
		OS_ASSERT(compiled_file_exist);
		Core::Program * prog = new (malloc(sizeof(Core::Program) OS_DBG_FILEPOS)) Core::Program(this);
		prog->filename = compiled_filename;

		Core::FileStreamReader prog_file_reader(this, compiled_filename);
		Core::MemStreamWriter prog_file_data(this);
		prog_file_data.writeFromStream(&prog_file_reader);
		Core::MemStreamReader prog_reader(NULL, prog_file_data.buffer.buf, prog_file_data.getSize());

		String debug_info_filename = getDebugInfoFilename(filename);
		if(isFileExist(debug_info_filename)){
			Core::FileStreamReader debug_info_file_reader(this, debug_info_filename);
			Core::MemStreamWriter debug_info_file_data(this);
			debug_info_file_data.writeFromStream(&debug_info_file_reader);
			Core::MemStreamReader debug_info_reader(NULL, debug_info_file_data.buffer.buf, debug_info_file_data.getSize());
			if(!prog->loadFromStream(&prog_reader, &debug_info_reader)){
				prog->release();
				return false;
			}
		}else if(!prog->loadFromStream(&prog_reader, NULL)){
			prog->release();
			return false;
		}
		prog->pushStartFunction();
		prog->release();
		return true;
	}

	Core::FileStreamReader file(this, filename);
	if(!file.f){
		core->error(OS_E_ERROR, String::format(this, OS_TEXT("error open filename %s"), p_filename.toChar()));
		return false;
	}

	Core::MemStreamWriter file_data(this);
	file_data.writeFromStream(&file);

	Core::Tokenizer tokenizer(this);
	tokenizer.parseText((OS_CHAR*)file_data.buffer.buf, file_data.buffer.count, filename);

	Core::Compiler compiler(&tokenizer);
	return compiler.compile();
}

bool OS::compile(const String& str)
{
	if(str.getDataSize() == 0){
		return false;
	}
	Core::Tokenizer tokenizer(this);
	tokenizer.parseText(str.toChar(), str.getLen(), String(this));

	Core::Compiler compiler(&tokenizer);
	return compiler.compile();
}

bool OS::compile()
{
	String str = toString(-1);
	pop(1);
	return compile(str);
}

int OS::call(int params, int ret_values)
{
	return core->call(params, ret_values);
}

int OS::eval(const OS_CHAR * str, int params, int ret_values)
{
	return eval(String(this, str), params, ret_values);
}

int OS::eval(const String& str, int params, int ret_values)
{
	compile(str);
	pushNull();
	move(-2, 2, -2-params);
	return core->call(params, ret_values);
}

int OS::require(const OS_CHAR * filename, bool required, int ret_values)
{
	return require(String(this, filename), required, ret_values);
}

int OS::require(const String& filename, bool required, int ret_values)
{
	getGlobal(OS_TEXT("require"));
	pushGlobals();
	pushString(filename);
	pushBool(required);
	return call(2, ret_values);
}

int OS::getSetting(OS_ESettings setting)
{
	switch(setting){
	case OS_SETTING_CREATE_DEBUG_INFO:
		return core->settings.create_debug_info;

	case OS_SETTING_CREATE_DEBUG_OPCODES:
		return core->settings.create_debug_opcodes;

	case OS_SETTING_CREATE_COMPILED_FILE:
		return core->settings.create_compiled_file;

	case OS_SETTING_PRIMARY_COMPILED_FILE:
		return core->settings.primary_compiled_file;
	}
	return -1;
}

int OS::setSetting(OS_ESettings setting, int value)
{
	struct Lib {
		static int ret(bool& cur_value, int new_value)
		{
			int old = cur_value;
			cur_value = new_value ? true : false;
			return old;
		}	
	};

	switch(setting){
	case OS_SETTING_CREATE_DEBUG_INFO:
		return Lib::ret(core->settings.create_debug_info, value);

	case OS_SETTING_CREATE_DEBUG_OPCODES:
		return Lib::ret(core->settings.create_debug_opcodes, value);

	case OS_SETTING_CREATE_COMPILED_FILE:
		return Lib::ret(core->settings.create_compiled_file, value);

	case OS_SETTING_PRIMARY_COMPILED_FILE:
		return Lib::ret(core->settings.primary_compiled_file, value);
	}
	return -1;
}

int OS::gc()
{
	return core->gcStep();
}

void OS::gcFull()
{
	core->gcFull();
}

void OS::triggerError(int code, const OS_CHAR * message)
{
	core->error(code, message);
}

void OS::triggerError(int code, const String& message)
{
	core->error(code, message);
}

void OS::triggerError(const OS_CHAR * message)
{
	core->error(OS_E_ERROR, message);
}

void OS::triggerError(const String& message)
{
	core->error(OS_E_ERROR, message);
}

// =====================================================================
// =====================================================================
// =====================================================================

#define OS_QSORT_CUTOFF 8

static void qsortSwap(char *a, char *b, unsigned width)
{
	char tmp;

	if(a != b) {
		if(width == sizeof(void*)){
			void * tmp = *(void**)a;
			*(void**)a = *(void**)b;
			*(void**)b = tmp;
			return;
		}
		if(width >= 16 && width <= 256){
			void * tmp = alloca(width);
			OS_MEMCPY(tmp, a, width);
			OS_MEMCPY(a, b, width);
			OS_MEMCPY(b, tmp, width);
			return;
		}
		while(width--){
			tmp = *a;
			*a++ = *b;
			*b++ = tmp;
		}
	}
}

static void qsortShortsort(OS * os, char *lo, char *hi, unsigned width, int (*comp)(OS*, const void *, const void *, void*), void * user_params)
{
	char *p, *max;

	while (hi > lo) {
		max = lo;
		for (p = lo + width; p <= hi; p += width) if (comp(os, p, max, user_params) > 0) max = p;
		qsortSwap(max, hi, width);
		hi -= width;
	}
}

void OS::qsort(void *base, unsigned num, unsigned width, int (*comp)(OS*, const void *, const void *, void*), void * user_params)
{
	char *lo, *hi;
	char *mid;
	char *l, *h;
	unsigned size;
	char *lostk[30], *histk[30];
	int stkptr;

	if (num < 2 || width == 0) return;
	stkptr = 0;

	lo = (char*)base;
	hi = (char*)base + width * (num - 1);

recurse:
	size = (hi - lo) / width + 1;

	if (size <= OS_QSORT_CUTOFF) {
		qsortShortsort(this, lo, hi, width, comp, user_params);
	} else {
		mid = lo + (size / 2) * width;
		qsortSwap(mid, lo, width);

		l = lo;
		h = hi + width;

		for (;;) {
			do { l += width; } while (l <= hi && comp(this, l, lo, user_params) <= 0);
			do { h -= width; } while (h > lo && comp(this, h, lo, user_params) >= 0);
			if (h < l) break;
			qsortSwap(l, h, width);
		}

		qsortSwap(lo, h, width);

		if (h - 1 - lo >= hi - l) {
			if (lo + width < h) {
				lostk[stkptr] = lo;
				histk[stkptr] = h - width;
				++stkptr;
			}

			if (l < hi) {
				lo = l;
				goto recurse;
			}
		} else {
			if (l < hi) {
				lostk[stkptr] = l;
				histk[stkptr] = hi;
				++stkptr;
			}

			if (lo + width < h) {
				hi = h - width;
				goto recurse;
			}
		}
	}

	--stkptr;
	if (stkptr >= 0) {
		lo = lostk[stkptr];
		hi = histk[stkptr];
		goto recurse;
	}
}

// =====================================================================
// =====================================================================
// =====================================================================

static FunctionDataChain * function_data_first = NULL;

FunctionDataChain::FunctionDataChain()
{ 
	next = function_data_first;
	function_data_first = this;
}
FunctionDataChain::~FunctionDataChain()
{
}

void ObjectScript::finalizeAllBinds()
{
	while(function_data_first){
		FunctionDataChain * cur = function_data_first;
		function_data_first = cur->next;
		delete cur;
	}
}

struct FunctionDataFinalizer
{
	~FunctionDataFinalizer(){ ObjectScript::finalizeAllBinds(); }
} __functionDataFinalizer__;

// =====================================================================
// =====================================================================
// =====================================================================
