// m018: the AI protocol's JSON subset. cwd-independent.

#include "core/Json.h"

#include <cstdio>
#include <string>

namespace
{

int gFailures = 0;

void Check(bool aCondition, const char *aWhat)
{
	if (aCondition)
		std::printf("ok   %s\n", aWhat);
	else
	{
		std::printf("FAIL %s\n", aWhat);
		++gFailures;
	}
}

} // namespace

int main()
{
	ahkdeck::Json value;
	std::string error;

	const std::string text = "{\"a\":[1,true,null,\"x\\n\\\"y\\\"\"],\"b\":{\"c\":-2.5},\"u\":\"\\u4f60\\u597d\"}";
	Check(ahkdeck::Json::Parse(text, value, error), "parse nested document");
	Check(value.IsObject() && value.Size() == 3, "object size");
	Check(value["a"].IsArray() && value["a"].Size() == 4, "array size");
	Check(value["a"][0].AsInt() == 1, "array number");
	Check(value["a"][1].AsBool(), "array bool");
	Check(value["a"][2].IsNull(), "array null");
	Check(value["a"][3].AsString() == "x\n\"y\"", "string escapes");
	Check(value["b"]["c"].AsNumber() == -2.5, "nested number");
	Check(value["u"].AsString() == "\xE4\xBD\xA0\xE5\xA5\xBD", "unicode escape -> UTF-8");
	Check(value["missing"].IsNull(), "missing member is null");

	// Dump -> Parse round-trip.
	{
		const std::string dumped = value.Dump();
		ahkdeck::Json again;
		Check(ahkdeck::Json::Parse(dumped, again, error), "parse own dump");
		Check(again["a"][3].AsString() == value["a"][3].AsString(), "round-trip string");
		Check(again["b"]["c"].AsNumber() == -2.5, "round-trip number");
	}

	// Empty containers.
	{
		ahkdeck::Json emptyArray;
		Check(ahkdeck::Json::Parse("[]", emptyArray, error), "parse empty array");
		Check(emptyArray.IsArray() && emptyArray.Size() == 0, "empty array type");
		Check(emptyArray.Dump() == "[]", "empty array dump");

		ahkdeck::Json emptyObject;
		Check(ahkdeck::Json::Parse("{}", emptyObject, error), "parse empty object");
		Check(emptyObject.IsObject() && emptyObject.Size() == 0, "empty object type");
		Check(emptyObject.Dump() == "{}", "empty object dump");
	}

	// Building a request-shaped value.
	{
		ahkdeck::Json request = ahkdeck::Json::MakeObject();
		request.Set("model", ahkdeck::Json("mock"));
		ahkdeck::Json tools = ahkdeck::Json::MakeArray();
		ahkdeck::Json tool = ahkdeck::Json::MakeObject();
		tool.Set("name", ahkdeck::Json("run_script"));
		tools.Push(std::move(tool));
		request.Set("tools", std::move(tools));
		Check(request.Dump() == "{\"model\":\"mock\",\"tools\":[{\"name\":\"run_script\"}]}", "built JSON shape");
	}

	// Surrogate pair.
	{
		ahkdeck::Json emoji;
		Check(ahkdeck::Json::Parse("\"\\ud83d\\ude00\"", emoji, error), "parse surrogate pair");
		Check(!emoji.AsString().empty(), "surrogate pair decoded");
	}

	// Failures are errors, not empty values or crashes.
	{
		ahkdeck::Json bad;
		Check(!ahkdeck::Json::Parse("", bad, error), "empty input rejected");
		Check(!ahkdeck::Json::Parse("{", bad, error), "truncated object rejected");
		Check(!ahkdeck::Json::Parse("{\"a\":}", bad, error), "missing value rejected");
		Check(!ahkdeck::Json::Parse("tru", bad, error), "bad literal rejected");
		Check(!ahkdeck::Json::Parse("{} x", bad, error), "trailing garbage rejected");
	}

	if (gFailures)
	{
		std::printf("json: %d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("json: all checks passed\n");
	return 0;
}
