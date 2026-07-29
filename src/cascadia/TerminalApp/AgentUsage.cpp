// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentUsage.h"

#include <stdexcept>

namespace
{
    constexpr size_t MaxMetricIdLength = 64;
    constexpr size_t MaxDecimalTextLength = 64;
    constexpr size_t MaxUnitIdLength = 16;
    constexpr size_t MaxScopeLength = 32;
    constexpr size_t MaxSourceLength = 32;
    constexpr int64_t MaxFormattedIntegerDigits = 512;

    std::string requiredString(const Json::Value& item, const char* key, const size_t maxLength)
    {
        const auto& value = item[key];
        if (!value.isString())
        {
            throw std::invalid_argument{ std::string{ "usage item requires string field: " } + key };
        }
        auto text = value.asString();
        if (text.empty() || text.size() > maxLength)
        {
            throw std::invalid_argument{ std::string{ "usage item string length is invalid: " } + key };
        }
        return text;
    }

    bool isDecimalText(const std::string_view text)
    {
        size_t index = 0;
        auto consumeDigits = [&]() {
            const auto start = index;
            while (index < text.size() && text[index] >= '0' && text[index] <= '9')
            {
                ++index;
            }
            return index > start;
        };

        if (!consumeDigits())
        {
            return false;
        }
        if (index < text.size() && text[index] == '.')
        {
            ++index;
            if (!consumeDigits())
            {
                return false;
            }
        }
        if (index < text.size() && (text[index] == 'e' || text[index] == 'E'))
        {
            ++index;
            if (index < text.size() && (text[index] == '+' || text[index] == '-'))
            {
                ++index;
            }
            if (!consumeDigits())
            {
                return false;
            }
        }
        return index == text.size();
    }

    std::wstring formatCostAmount(const std::string_view text)
    {
        const auto exponentMarker = text.find_first_of("eE");
        const auto mantissa = text.substr(0, exponentMarker);
        const auto decimalPoint = mantissa.find('.');
        const auto integerDigits = decimalPoint == std::string_view::npos ? mantissa.size() : decimalPoint;

        std::string digits;
        digits.reserve(mantissa.size());
        for (const auto ch : mantissa)
        {
            if (ch != '.')
            {
                digits.push_back(ch);
            }
        }

        int64_t exponent = 0;
        if (exponentMarker != std::string_view::npos)
        {
            auto index = exponentMarker + 1;
            const auto negative = index < text.size() && text[index] == '-';
            if (index < text.size() && (text[index] == '+' || text[index] == '-'))
            {
                ++index;
            }
            for (; index < text.size(); ++index)
            {
                const auto digit = static_cast<int64_t>(text[index] - '0');
                exponent = std::min<int64_t>(MaxFormattedIntegerDigits + 1, exponent * 10 + digit);
            }
            if (negative)
            {
                exponent = -exponent;
            }
        }

        const auto decimalPosition = static_cast<int64_t>(integerDigits) + exponent;
        const auto firstNonZero = digits.find_first_not_of('0');
        if (firstNonZero == std::string::npos)
        {
            return L"0.00";
        }

        // Positive values below one cent should not be visually rounded to zero.
        if (static_cast<int64_t>(firstNonZero) > decimalPosition + 1)
        {
            return L"<0.01";
        }

        // Standard ACP cost originates as a finite f64, so its fixed form is
        // bounded. Keep the UI safe if a synthetic normalized event bypasses
        // that boundary with an extreme exponent.
        if (decimalPosition > MaxFormattedIntegerDigits)
        {
            return til::u8u16(text);
        }

        const auto digitAt = [&](const int64_t position) {
            return position >= 0 && position < static_cast<int64_t>(digits.size()) ?
                       digits[static_cast<size_t>(position)] :
                       '0';
        };

        std::string integer;
        if (decimalPosition > 0)
        {
            integer.reserve(static_cast<size_t>(decimalPosition));
            for (int64_t position = 0; position < decimalPosition; ++position)
            {
                integer.push_back(digitAt(position));
            }
            const auto firstSignificant = integer.find_first_not_of('0');
            integer.erase(0, firstSignificant == std::string::npos ? integer.size() - 1 : firstSignificant);
        }
        else
        {
            integer = "0";
        }

        std::string rounded = integer;
        rounded.push_back(digitAt(decimalPosition));
        rounded.push_back(digitAt(decimalPosition + 1));
        if (digitAt(decimalPosition + 2) >= '5')
        {
            auto position = rounded.size();
            while (position > 0 && rounded[position - 1] == '9')
            {
                rounded[--position] = '0';
            }
            if (position == 0)
            {
                rounded.insert(rounded.begin(), '1');
            }
            else
            {
                ++rounded[position - 1];
            }
        }

        rounded.insert(rounded.end() - 2, '.');
        return til::u8u16(rounded);
    }

    std::vector<TerminalApp::AgentUsage::PrimaryDisplayItem> buildPrimaryDisplayItems(
        const std::vector<TerminalApp::AgentUsage::Item>& items,
        const std::wstring_view tokensUnit)
    {
        using namespace TerminalApp::AgentUsage;

        std::vector<PrimaryDisplayItem> displayItems;
        displayItems.reserve(std::min(items.size(), MaxPrimaryItems));
        for (const auto metricId : { "acp.context.window", "acp.billing.cost" })
        {
            const auto item = std::ranges::find(items, metricId, &Item::metricId);
            if (item == items.end() || item->stale || displayItems.size() == MaxPrimaryItems)
            {
                continue;
            }

            if (item->metricId == "acp.context.window" && item->limitDecimalText)
            {
                auto text = til::u8u16(item->valueDecimalText);
                text += L" / ";
                text += til::u8u16(*item->limitDecimalText);
                text += L" ";
                text += tokensUnit;
                displayItems.emplace_back(PrimaryDisplayItem{ .text = std::move(text) });
            }
            else
            {
                auto fullText = til::u8u16(item->valueDecimalText);
                fullText += L" ";
                fullText += til::u8u16(item->unitId);
                auto text = formatCostAmount(item->valueDecimalText);
                text += L" ";
                text += til::u8u16(item->unitId);
                displayItems.emplace_back(PrimaryDisplayItem{
                    .text = std::move(text),
                    .fullText = std::move(fullText),
                });
            }
        }
        return displayItems;
    }
}

namespace TerminalApp::AgentUsage
{
    std::vector<Item> Parse(const Json::Value& usage)
    {
        if (usage.isNull())
        {
            return {};
        }
        if (!usage.isObject() || !usage.isMember("items") || !usage["items"].isArray())
        {
            throw std::invalid_argument{ "usage must be null or an object containing an items array" };
        }

        const auto& items = usage["items"];
        if (items.size() > MaxItems)
        {
            throw std::invalid_argument{ "usage contains too many items" };
        }

        std::vector<Item> parsed;
        parsed.reserve(items.size());
        for (const auto& item : items)
        {
            if (!item.isObject())
            {
                throw std::invalid_argument{ "usage item must be an object" };
            }

            Item result;
            result.metricId = requiredString(item, "metric_id", MaxMetricIdLength);
            result.valueDecimalText = requiredString(item, "value_decimal_text", MaxDecimalTextLength);
            if (!isDecimalText(result.valueDecimalText))
            {
                throw std::invalid_argument{ "usage value_decimal_text is invalid" };
            }
            if (item.isMember("limit_decimal_text"))
            {
                result.limitDecimalText = requiredString(item, "limit_decimal_text", MaxDecimalTextLength);
                if (!isDecimalText(*result.limitDecimalText))
                {
                    throw std::invalid_argument{ "usage limit_decimal_text is invalid" };
                }
            }
            result.unitId = requiredString(item, "unit_id", MaxUnitIdLength);
            result.scope = requiredString(item, "scope", MaxScopeLength);
            result.source = requiredString(item, "source", MaxSourceLength);
            if (!item.isMember("stale") || !item["stale"].isBool())
            {
                throw std::invalid_argument{ "usage item requires bool field: stale" };
            }
            result.stale = item["stale"].asBool();
            parsed.emplace_back(std::move(result));
        }
        return parsed;
    }

    void UpdateCache(std::vector<Item>& cache, const Json::Value& usage)
    {
        auto parsed = Parse(usage);
        cache = std::move(parsed);
    }

    std::vector<std::wstring> BuildPrimaryDisplayTexts(
        const std::vector<Item>& items,
        const std::wstring_view tokensUnit)
    {
        const auto displayItems = buildPrimaryDisplayItems(items, tokensUnit);
        std::vector<std::wstring> texts;
        texts.reserve(displayItems.size());
        for (const auto& item : displayItems)
        {
            texts.emplace_back(item.text);
        }
        return texts;
    }

    PrimaryDisplay BuildPrimaryDisplay(
        const std::vector<Item>& items,
        const std::wstring_view tokensUnit)
    {
        auto displayItems = buildPrimaryDisplayItems(items, tokensUnit);
        const auto visible = !displayItems.empty();
        return PrimaryDisplay{
            .items = std::move(displayItems),
            .visible = visible,
        };
    }
}
