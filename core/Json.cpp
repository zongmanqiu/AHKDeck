#include "core/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ahkdeck
{
namespace
{

const Json &NullJson()
{
	static const Json null;
	return null;
}

void EscapeString(const std::string &aIn, std::string &aOut)
{
	aOut += '"';
	for (unsigned char c : aIn)
	{
		switch (c)
		{
		case '"': aOut += "\\\""; break;
		case '\\': aOut += "\\\\"; break;
		case '\b': aOut += "\\b"; break;
		case '\f': aOut += "\\f"; break;
		case '\n': aOut += "\\n"; break;
		case '\r': aOut += "\\r"; break;
		case '\t': aOut += "\\t"; break;
		default:
			if (c < 0x20)
			{
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				aOut += buf;
			}
			else
				aOut += (char)c;
			break;
		}
	}
	aOut += '"';
}

void AppendUtf8(unsigned int aCodePoint, std::string &aOut)
{
	if (aCodePoint <= 0x7F)
		aOut += (char)aCodePoint;
	else if (aCodePoint <= 0x7FF)
	{
		aOut += (char)(0xC0 | (aCodePoint >> 6));
		aOut += (char)(0x80 | (aCodePoint & 0x3F));
	}
	else if (aCodePoint <= 0xFFFF)
	{
		aOut += (char)(0xE0 | (aCodePoint >> 12));
		aOut += (char)(0x80 | ((aCodePoint >> 6) & 0x3F));
		aOut += (char)(0x80 | (aCodePoint & 0x3F));
	}
	else
	{
		aOut += (char)(0xF0 | (aCodePoint >> 18));
		aOut += (char)(0x80 | ((aCodePoint >> 12) & 0x3F));
		aOut += (char)(0x80 | ((aCodePoint >> 6) & 0x3F));
		aOut += (char)(0x80 | (aCodePoint & 0x3F));
	}
}

class Parser
{
public:
	explicit Parser(const std::string &aText) : mText(aText) {}

	bool Parse(Json &aOut, std::string &aError)
	{
		SkipSpace();
		if (!ParseValue(aOut, 0))
		{
			aError = mError;
			return false;
		}
		SkipSpace();
		if (mPos != mText.size())
		{
			aError = "trailing characters after JSON value";
			return false;
		}
		return true;
	}

private:
	void SkipSpace()
	{
		while (mPos < mText.size())
		{
			const char c = mText[mPos];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
				++mPos;
			else
				break;
		}
	}

	bool Fail(const char *aReason)
	{
		if (mError.empty())
		{
			char buf[96];
			std::snprintf(buf, sizeof(buf), "%s at offset %zu", aReason, mPos);
			mError = buf;
		}
		return false;
	}

	bool Match(const char *aLiteral)
	{
		const size_t length = std::strlen(aLiteral);
		if (mText.compare(mPos, length, aLiteral) != 0)
			return false;
		mPos += length;
		return true;
	}

	bool ParseValue(Json &aOut, int aDepth)
	{
		if (aDepth > 64)
			return Fail("nesting too deep");
		if (mPos >= mText.size())
			return Fail("unexpected end of JSON");

		const char c = mText[mPos];
		if (c == '{')
			return ParseObject(aOut, aDepth);
		if (c == '[')
			return ParseArray(aOut, aDepth);
		if (c == '"')
		{
			std::string value;
			if (!ParseString(value))
				return false;
			aOut = Json(value);
			return true;
		}
		if (c == 't')
		{
			if (!Match("true"))
				return Fail("invalid literal");
			aOut = Json(true);
			return true;
		}
		if (c == 'f')
		{
			if (!Match("false"))
				return Fail("invalid literal");
			aOut = Json(false);
			return true;
		}
		if (c == 'n')
		{
			if (!Match("null"))
				return Fail("invalid literal");
			aOut = Json();
			return true;
		}
		if (c == '-' || (c >= '0' && c <= '9'))
			return ParseNumber(aOut);
		return Fail("unexpected character");
	}

	bool ParseNumber(Json &aOut)
	{
		char *end = nullptr;
		const double value = std::strtod(mText.c_str() + mPos, &end);
		if (end == mText.c_str() + mPos)
			return Fail("invalid number");
		mPos = (size_t)(end - mText.c_str());
		aOut = Json(value);
		return true;
	}

	bool ParseHex4(unsigned int &aOut)
	{
		if (mPos + 4 > mText.size())
			return Fail("truncated \\u escape");
		unsigned int value = 0;
		for (int i = 0; i < 4; ++i)
		{
			const char c = mText[mPos++];
			value <<= 4;
			if (c >= '0' && c <= '9')
				value |= (unsigned int)(c - '0');
			else if (c >= 'a' && c <= 'f')
				value |= (unsigned int)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				value |= (unsigned int)(c - 'A' + 10);
			else
				return Fail("invalid \\u escape");
		}
		aOut = value;
		return true;
	}

	bool ParseString(std::string &aOut)
	{
		if (mPos >= mText.size() || mText[mPos] != '"')
			return Fail("expected a string");
		++mPos;
		while (mPos < mText.size())
		{
			const char c = mText[mPos++];
			if (c == '"')
				return true;
			if (c != '\\')
			{
				aOut += c;
				continue;
			}
			if (mPos >= mText.size())
				return Fail("truncated escape");
			const char escape = mText[mPos++];
			switch (escape)
			{
			case '"': aOut += '"'; break;
			case '\\': aOut += '\\'; break;
			case '/': aOut += '/'; break;
			case 'b': aOut += '\b'; break;
			case 'f': aOut += '\f'; break;
			case 'n': aOut += '\n'; break;
			case 'r': aOut += '\r'; break;
			case 't': aOut += '\t'; break;
			case 'u':
			{
				unsigned int codePoint = 0;
				if (!ParseHex4(codePoint))
					return false;
				if (codePoint >= 0xD800 && codePoint <= 0xDBFF && mPos + 6 <= mText.size()
					&& mText[mPos] == '\\' && mText[mPos + 1] == 'u')
				{
					mPos += 2;
					unsigned int low = 0;
					if (!ParseHex4(low))
						return false;
					if (low >= 0xDC00 && low <= 0xDFFF)
						codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
					else
						AppendUtf8(0xFFFD, aOut);
				}
				AppendUtf8(codePoint, aOut);
				break;
			}
			default:
				return Fail("unknown escape");
			}
		}
		return Fail("unterminated string");
	}

	bool ParseArray(Json &aOut, int aDepth)
	{
		++mPos; // [
		std::vector<Json> items;
		SkipSpace();
		if (mPos < mText.size() && mText[mPos] == ']')
		{
			++mPos;
			aOut = Json::MakeArray();
			return true;
		}
		while (true)
		{
			Json item;
			SkipSpace();
			if (!ParseValue(item, aDepth + 1))
				return false;
			items.push_back(std::move(item));
			SkipSpace();
			if (mPos >= mText.size())
				return Fail("unterminated array");
			if (mText[mPos] == ',')
			{
				++mPos;
				continue;
			}
			if (mText[mPos] == ']')
			{
				++mPos;
				break;
			}
			return Fail("expected ',' or ']'");
		}
		Json result = Json::MakeArray();
		for (auto &item : items)
			result.Push(std::move(item));
		aOut = std::move(result);
		return true;
	}

	bool ParseObject(Json &aOut, int aDepth)
	{
		++mPos; // {
		Json result = Json::MakeObject();
		SkipSpace();
		if (mPos < mText.size() && mText[mPos] == '}')
		{
			++mPos;
			aOut = result;
			return true;
		}
		while (true)
		{
			SkipSpace();
			std::string key;
			if (!ParseString(key))
				return false;
			SkipSpace();
			if (mPos >= mText.size() || mText[mPos] != ':')
				return Fail("expected ':'");
			++mPos;
			SkipSpace();
			Json value;
			if (!ParseValue(value, aDepth + 1))
				return false;
			result.Set(key, std::move(value));
			SkipSpace();
			if (mPos >= mText.size())
				return Fail("unterminated object");
			if (mText[mPos] == ',')
			{
				++mPos;
				continue;
			}
			if (mText[mPos] == '}')
			{
				++mPos;
				break;
			}
			return Fail("expected ',' or '}'");
		}
		aOut = std::move(result);
		return true;
	}

	const std::string &mText;
	size_t mPos = 0;
	std::string mError;
};

} // namespace

Json::Json(bool aValue) : mType(Type::Bool), mBool(aValue) {}
Json::Json(double aValue) : mType(Type::Number), mNumber(aValue) {}
Json::Json(const char *aValue) : mType(Type::String), mString(aValue ? aValue : "") {}
Json::Json(const std::string &aValue) : mType(Type::String), mString(aValue) {}

bool Json::AsBool(bool aDefault) const
{
	return mType == Type::Bool ? mBool : aDefault;
}

double Json::AsNumber(double aDefault) const
{
	return mType == Type::Number ? mNumber : aDefault;
}

int Json::AsInt(int aDefault) const
{
	return mType == Type::Number ? (int)mNumber : aDefault;
}

const std::string &Json::AsString() const
{
	static const std::string empty;
	return mType == Type::String ? mString : empty;
}

size_t Json::Size() const
{
	if (mType == Type::Array)
		return mArray.size();
	if (mType == Type::Object)
		return mObject.size();
	return 0;
}

const Json &Json::operator[](const std::string &aKey) const
{
	if (mType != Type::Object)
		return NullJson();
	for (const auto &entry : mObject)
	{
		if (entry.first == aKey)
			return entry.second;
	}
	return NullJson();
}

const Json &Json::operator[](size_t aIndex) const
{
	if (mType != Type::Array || aIndex >= mArray.size())
		return NullJson();
	return mArray[aIndex];
}

bool Json::Has(const std::string &aKey) const
{
	if (mType != Type::Object)
		return false;
	for (const auto &entry : mObject)
	{
		if (entry.first == aKey)
			return true;
	}
	return false;
}

Json &Json::Set(const std::string &aKey, Json aValue)
{
	if (mType != Type::Object)
	{
		mType = Type::Object;
		mObject.clear();
	}
	for (auto &entry : mObject)
	{
		if (entry.first == aKey)
		{
			entry.second = std::move(aValue);
			return *this;
		}
	}
	mObject.emplace_back(aKey, std::move(aValue));
	return *this;
}

Json &Json::Push(Json aValue)
{
	if (mType != Type::Array)
	{
		mType = Type::Array;
		mArray.clear();
	}
	mArray.push_back(std::move(aValue));
	return *this;
}

Json Json::MakeArray()
{
	Json value;
	value.mType = Type::Array;
	return value;
}

Json Json::MakeObject()
{
	Json value;
	value.mType = Type::Object;
	return value;
}

std::string Json::Dump() const
{
	std::string out;
	DumpTo(out);
	return out;
}

std::string Json::DumpPretty() const
{
	std::string out;
	DumpToPretty(out, 0);
	return out;
}

void Json::DumpToPretty(std::string &aOut, int aIndent) const
{
	if (mType != Type::Object && mType != Type::Array)
	{
		DumpTo(aOut);
		return;
	}

	auto indent = [&aOut](int aCount) {
		aOut.append((size_t)(aCount > 0 ? aCount : 0), ' ');
	};

	if (mType == Type::Array)
	{
		if (mArray.empty())
		{
			aOut += "[]";
			return;
		}
		aOut += "[\n";
		for (size_t i = 0; i < mArray.size(); ++i)
		{
			indent(aIndent + 2);
			mArray[i].DumpToPretty(aOut, aIndent + 2);
			if (i + 1 < mArray.size())
				aOut += ',';
			aOut += '\n';
		}
		indent(aIndent);
		aOut += ']';
		return;
	}

	if (mObject.empty())
	{
		aOut += "{}";
		return;
	}
	aOut += "{\n";
	for (size_t i = 0; i < mObject.size(); ++i)
	{
		indent(aIndent + 2);
		EscapeString(mObject[i].first, aOut);
		aOut += ": ";
		mObject[i].second.DumpToPretty(aOut, aIndent + 2);
		if (i + 1 < mObject.size())
			aOut += ',';
		aOut += '\n';
	}
	indent(aIndent);
	aOut += '}';
}

void Json::DumpTo(std::string &aOut) const
{
	switch (mType)
	{
	case Type::Null:
		aOut += "null";
		break;
	case Type::Bool:
		aOut += mBool ? "true" : "false";
		break;
	case Type::Number:
	{
		if (!std::isfinite(mNumber))
		{
			aOut += "null";
			break;
		}
		char buf[40];
		if (mNumber == (double)(long long)mNumber)
			std::snprintf(buf, sizeof(buf), "%lld", (long long)mNumber);
		else
		{
			// Shortest representation that still round-trips: avoids turning
			// 0.77 into 0.77000000000000002 in user-edited settings.json.
			for (int precision = 15; precision <= 17; ++precision)
			{
				std::snprintf(buf, sizeof(buf), "%.*g", precision, mNumber);
				if (std::strtod(buf, nullptr) == mNumber)
					break;
			}
		}
		aOut += buf;
		break;
	}
	case Type::String:
		EscapeString(mString, aOut);
		break;
	case Type::Array:
	{
		aOut += '[';
		for (size_t i = 0; i < mArray.size(); ++i)
		{
			if (i)
				aOut += ',';
			mArray[i].DumpTo(aOut);
		}
		aOut += ']';
		break;
	}
	case Type::Object:
	{
		aOut += '{';
		for (size_t i = 0; i < mObject.size(); ++i)
		{
			if (i)
				aOut += ',';
			EscapeString(mObject[i].first, aOut);
			aOut += ':';
			mObject[i].second.DumpTo(aOut);
		}
		aOut += '}';
		break;
	}
	}
}

bool Json::Parse(const std::string &aText, Json &aOut, std::string &aError)
{
	Parser parser(aText);
	return parser.Parse(aOut, aError);
}

} // namespace ahkdeck
