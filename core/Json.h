// Minimal JSON for the AI layer (m018).
//
// The deck must stay small: the AI protocol needs objects/arrays/strings and
// nothing else, so this is a ~300-line recursive-descent parser instead of a
// third-party header. It never throws: failures come back as a bool + reason.

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ahkdeck
{

class Json
{
public:
	enum class Type
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	Json() = default;
	Json(bool aValue);                 // NOLINT(google-explicit-constructor): JSON literals
	Json(double aValue);               // NOLINT
	Json(const char *aValue);          // NOLINT
	Json(const std::string &aValue);   // NOLINT

	Type type() const { return mType; }
	bool IsNull() const { return mType == Type::Null; }
	bool IsBool() const { return mType == Type::Bool; }
	bool IsNumber() const { return mType == Type::Number; }
	bool IsString() const { return mType == Type::String; }
	bool IsArray() const { return mType == Type::Array; }
	bool IsObject() const { return mType == Type::Object; }

	bool AsBool(bool aDefault = false) const;
	double AsNumber(double aDefault = 0.0) const;
	int AsInt(int aDefault = 0) const;
	const std::string &AsString() const;

	size_t Size() const;
	const Json &operator[](const std::string &aKey) const; // object member or null
	const Json &operator[](size_t aIndex) const;           // array element or null
	bool Has(const std::string &aKey) const;

	// Object members in insertion order (empty for non-objects). Used by the
	// config layer to merge advanced fields without dropping unknowns.
	const std::vector<std::pair<std::string, Json>> &Items() const { return mObject; }

	Json &Set(const std::string &aKey, Json aValue); // becomes an object
	Json &Push(Json aValue);                         // becomes an array

	static Json MakeArray();
	static Json MakeObject();

	std::string Dump() const;
	std::string DumpPretty() const;
	static bool Parse(const std::string &aText, Json &aOut, std::string &aError);

private:
	void DumpTo(std::string &aOut) const;
	void DumpToPretty(std::string &aOut, int aIndent) const;

	Type mType = Type::Null;
	bool mBool = false;
	double mNumber = 0.0;
	std::string mString;
	std::vector<Json> mArray;
	std::vector<std::pair<std::string, Json>> mObject;
};

} // namespace ahkdeck
