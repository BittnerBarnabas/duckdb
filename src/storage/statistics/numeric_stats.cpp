#include "duckdb/storage/statistics/numeric_stats.hpp"

#include "duckdb/common/field_writer.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

template <>
void NumericStats::Update<interval_t>(BaseStatistics &stats, interval_t new_value) {
}

template <>
void NumericStats::Update<list_entry_t>(BaseStatistics &stats, list_entry_t new_value) {
}

unique_ptr<BaseStatistics> NumericStats::CreateUnknown(LogicalType type) {
	auto result = BaseStatistics::Construct(std::move(type));
	result->InitializeUnknown();
	SetMin(*result, Value(type));
	SetMax(*result, Value(type));
	return result;
}

unique_ptr<BaseStatistics> NumericStats::CreateEmpty(LogicalType type) {
	auto result = BaseStatistics::Construct(std::move(type));
	result->InitializeEmpty();
	SetMin(*result, Value::MaximumValue(result->GetType()));
	SetMax(*result, Value::MinimumValue(result->GetType()));
	return result;
}

bool NumericStats::IsNumeric(const BaseStatistics &stats) {
	switch (stats.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::INT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	default:
		return false;
	}
}

NumericStatsData &NumericStats::GetDataUnsafe(BaseStatistics &stats) {
	D_ASSERT(NumericStats::IsNumeric(stats));
	return stats.stats_union.numeric_data;
}

const NumericStatsData &NumericStats::GetDataUnsafe(const BaseStatistics &stats) {
	D_ASSERT(NumericStats::IsNumeric(stats));
	return stats.stats_union.numeric_data;
}

void NumericStats::Merge(BaseStatistics &stats, const BaseStatistics &other) {
	if (other.GetType().id() == LogicalTypeId::VALIDITY) {
		return;
	}
	if (NumericStats::HasMin(other) && NumericStats::HasMin(stats)) {
		auto other_min = NumericStats::Min(other);
		if (other_min < NumericStats::Min(stats)) {
			NumericStats::SetMin(stats, other_min);
		}
	} else {
		NumericStats::SetMin(stats, Value());
	}
	if (NumericStats::HasMax(other) && NumericStats::HasMax(stats)) {
		auto other_max = NumericStats::Max(other);
		if (other_max > NumericStats::Max(stats)) {
			NumericStats::SetMax(stats, other_max);
		}
	} else {
		NumericStats::SetMax(stats, Value());
	}
}

FilterPropagateResult NumericStats::CheckZonemap(const BaseStatistics &stats, ExpressionType comparison_type,
                                                 const Value &constant) {
	if (constant.IsNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (!NumericStats::HasMin(stats) || !NumericStats::HasMax(stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto min_value = NumericStats::Min(stats);
	auto max_value = NumericStats::Max(stats);
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		if (constant == min_value && constant == max_value) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		} else if (constant >= min_value && constant <= max_value) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		} else {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
	case ExpressionType::COMPARE_NOTEQUAL:
		if (constant < min_value || constant > max_value) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		} else if (min_value == max_value && min_value == constant) {
			// corner case of a cluster with one numeric equal to the target constant
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		// X >= C
		// this can be true only if max(X) >= C
		// if min(X) >= C, then this is always true
		if (min_value >= constant) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		} else if (max_value >= constant) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		} else {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
	case ExpressionType::COMPARE_GREATERTHAN:
		// X > C
		// this can be true only if max(X) > C
		// if min(X) > C, then this is always true
		if (min_value > constant) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		} else if (max_value > constant) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		} else {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		// X <= C
		// this can be true only if min(X) <= C
		// if max(X) <= C, then this is always true
		if (max_value <= constant) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		} else if (min_value <= constant) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		} else {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
	case ExpressionType::COMPARE_LESSTHAN:
		// X < C
		// this can be true only if min(X) < C
		// if max(X) < C, then this is always true
		if (max_value < constant) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		} else if (min_value < constant) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		} else {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
	default:
		throw InternalException("Expression type in zonemap check not implemented");
	}
}

bool NumericStats::IsConstant(const BaseStatistics &stats) {
	return NumericStats::Max(stats) <= NumericStats::Min(stats);
}

void SetNumericValueInternal(const Value &input, const LogicalType &type, NumericValueUnion &val, bool &has_val) {
	if (input.IsNull()) {
		has_val = false;
		return;
	}
	if (input.type().InternalType() != type.InternalType()) {
		throw InternalException("SetMin or SetMax called with Value that does not match statistics' column value");
	}
	has_val = true;
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		val.value_.boolean = BooleanValue::Get(input);
		break;
	case PhysicalType::INT8:
		val.value_.tinyint = TinyIntValue::Get(input);
		break;
	case PhysicalType::INT16:
		val.value_.smallint = SmallIntValue::Get(input);
		break;
	case PhysicalType::INT32:
		val.value_.integer = IntegerValue::Get(input);
		break;
	case PhysicalType::INT64:
		val.value_.bigint = BigIntValue::Get(input);
		break;
	case PhysicalType::UINT8:
		val.value_.utinyint = UTinyIntValue::Get(input);
		break;
	case PhysicalType::UINT16:
		val.value_.usmallint = USmallIntValue::Get(input);
		break;
	case PhysicalType::UINT32:
		val.value_.uinteger = UIntegerValue::Get(input);
		break;
	case PhysicalType::UINT64:
		val.value_.ubigint = UBigIntValue::Get(input);
		break;
	case PhysicalType::INT128:
		val.value_.hugeint = HugeIntValue::Get(input);
		break;
	case PhysicalType::FLOAT:
		val.value_.float_ = FloatValue::Get(input);
		break;
	case PhysicalType::DOUBLE:
		val.value_.double_ = DoubleValue::Get(input);
		break;
	default:
		throw InternalException("Unsupported type for NumericStatistics::SetValueInternal");
	}
}

void NumericStats::SetMin(BaseStatistics &stats, const Value &new_min) {
	auto &data = NumericStats::GetDataUnsafe(stats);
	SetNumericValueInternal(new_min, stats.GetType(), data.min, data.has_min);
}

void NumericStats::SetMax(BaseStatistics &stats, const Value &new_max) {
	auto &data = NumericStats::GetDataUnsafe(stats);
	SetNumericValueInternal(new_max, stats.GetType(), data.max, data.has_max);
}

Value NumericValueUnionToValueInternal(const LogicalType &type, const NumericValueUnion &val) {
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		return Value::BOOLEAN(val.value_.boolean);
	case PhysicalType::INT8:
		return Value::TINYINT(val.value_.tinyint);
	case PhysicalType::INT16:
		return Value::SMALLINT(val.value_.smallint);
	case PhysicalType::INT32:
		return Value::INTEGER(val.value_.integer);
	case PhysicalType::INT64:
		return Value::BIGINT(val.value_.bigint);
	case PhysicalType::UINT8:
		return Value::UTINYINT(val.value_.utinyint);
	case PhysicalType::UINT16:
		return Value::USMALLINT(val.value_.usmallint);
	case PhysicalType::UINT32:
		return Value::UINTEGER(val.value_.uinteger);
	case PhysicalType::UINT64:
		return Value::UBIGINT(val.value_.ubigint);
	case PhysicalType::INT128:
		return Value::HUGEINT(val.value_.hugeint);
	case PhysicalType::FLOAT:
		return Value::FLOAT(val.value_.float_);
	case PhysicalType::DOUBLE:
		return Value::DOUBLE(val.value_.double_);
	default:
		throw InternalException("Unsupported type for NumericValueUnionToValue");
	}
}

Value NumericValueUnionToValue(const LogicalType &type, const NumericValueUnion &val) {
	Value result = NumericValueUnionToValueInternal(type, val);
	result.GetTypeMutable() = type;
	return result;
}

bool NumericStats::HasMin(const BaseStatistics &stats) {
	return NumericStats::GetDataUnsafe(stats).has_min;
}

bool NumericStats::HasMax(const BaseStatistics &stats) {
	return NumericStats::GetDataUnsafe(stats).has_max;
}

Value NumericStats::Min(const BaseStatistics &stats) {
	if (!NumericStats::HasMin(stats)) {
		throw InternalException("Min() called on statistics that does not have min");
	}
	return NumericValueUnionToValue(stats.GetType(), NumericStats::GetDataUnsafe(stats).min);
}

Value NumericStats::Max(const BaseStatistics &stats) {
	if (!NumericStats::HasMax(stats)) {
		throw InternalException("Max() called on statistics that does not have max");
	}
	return NumericValueUnionToValue(stats.GetType(), NumericStats::GetDataUnsafe(stats).max);
}

Value NumericStats::MinOrNull(const BaseStatistics &stats) {
	if (!NumericStats::HasMin(stats)) {
		return Value(stats.GetType());
	}
	return NumericStats::Min(stats);
}

Value NumericStats::MaxOrNull(const BaseStatistics &stats) {
	if (!NumericStats::HasMax(stats)) {
		return Value(stats.GetType());
	}
	return NumericStats::Max(stats);
}

void SerializeNumericStatsValue(const Value &val, FieldWriter &writer) {
	writer.WriteField<bool>(val.IsNull());
	if (val.IsNull()) {
		return;
	}
	switch (val.type().InternalType()) {
	case PhysicalType::BOOL:
		writer.WriteField<bool>(BooleanValue::Get(val));
		break;
	case PhysicalType::INT8:
		writer.WriteField<int8_t>(TinyIntValue::Get(val));
		break;
	case PhysicalType::INT16:
		writer.WriteField<int16_t>(SmallIntValue::Get(val));
		break;
	case PhysicalType::INT32:
		writer.WriteField<int32_t>(IntegerValue::Get(val));
		break;
	case PhysicalType::INT64:
		writer.WriteField<int64_t>(BigIntValue::Get(val));
		break;
	case PhysicalType::UINT8:
		writer.WriteField<int8_t>(UTinyIntValue::Get(val));
		break;
	case PhysicalType::UINT16:
		writer.WriteField<int16_t>(USmallIntValue::Get(val));
		break;
	case PhysicalType::UINT32:
		writer.WriteField<int32_t>(UIntegerValue::Get(val));
		break;
	case PhysicalType::UINT64:
		writer.WriteField<int64_t>(UBigIntValue::Get(val));
		break;
	case PhysicalType::INT128:
		writer.WriteField<hugeint_t>(HugeIntValue::Get(val));
		break;
	case PhysicalType::FLOAT:
		writer.WriteField<float>(FloatValue::Get(val));
		break;
	case PhysicalType::DOUBLE:
		writer.WriteField<double>(DoubleValue::Get(val));
		break;
	default:
		throw InternalException("Unsupported type for serializing numeric statistics");
	}
}

void NumericStats::Serialize(const BaseStatistics &stats, FieldWriter &writer) {
	SerializeNumericStatsValue(NumericStats::MinOrNull(stats), writer);
	SerializeNumericStatsValue(NumericStats::MaxOrNull(stats), writer);
}

Value DeserializeNumericStatsValue(const LogicalType &type, FieldReader &reader) {
	auto is_null = reader.ReadRequired<bool>();
	if (is_null) {
		return Value(type);
	}
	Value result;
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		result = Value::BOOLEAN(reader.ReadRequired<bool>());
		break;
	case PhysicalType::INT8:
		result = Value::TINYINT(reader.ReadRequired<int8_t>());
		break;
	case PhysicalType::INT16:
		result = Value::SMALLINT(reader.ReadRequired<int16_t>());
		break;
	case PhysicalType::INT32:
		result = Value::INTEGER(reader.ReadRequired<int32_t>());
		break;
	case PhysicalType::INT64:
		result = Value::BIGINT(reader.ReadRequired<int64_t>());
		break;
	case PhysicalType::UINT8:
		result = Value::UTINYINT(reader.ReadRequired<uint8_t>());
		break;
	case PhysicalType::UINT16:
		result = Value::USMALLINT(reader.ReadRequired<uint16_t>());
		break;
	case PhysicalType::UINT32:
		result = Value::UINTEGER(reader.ReadRequired<uint32_t>());
		break;
	case PhysicalType::UINT64:
		result = Value::UBIGINT(reader.ReadRequired<uint64_t>());
		break;
	case PhysicalType::INT128:
		result = Value::HUGEINT(reader.ReadRequired<hugeint_t>());
		break;
	case PhysicalType::FLOAT:
		result = Value::FLOAT(reader.ReadRequired<float>());
		break;
	case PhysicalType::DOUBLE:
		result = Value::DOUBLE(reader.ReadRequired<double>());
		break;
	default:
		throw InternalException("Unsupported type for deserializing numeric statistics");
	}
	result.Reinterpret(type);
	return result;
}

BaseStatistics NumericStats::Deserialize(FieldReader &reader, LogicalType type) {
	auto min = DeserializeNumericStatsValue(type, reader);
	auto max = DeserializeNumericStatsValue(type, reader);
	BaseStatistics result(std::move(type));
	NumericStats::SetMin(result, min);
	NumericStats::SetMax(result, max);
	return result;
}

string NumericStats::ToString(const BaseStatistics &stats) {
	return StringUtil::Format("[Min: %s, Max: %s]", NumericStats::MinOrNull(stats).ToString(),
	                          NumericStats::MaxOrNull(stats).ToString());
}

template <class T>
void NumericStats::TemplatedVerify(const BaseStatistics &stats, Vector &vector, const SelectionVector &sel,
                                   idx_t count) {
	UnifiedVectorFormat vdata;
	vector.ToUnifiedFormat(count, vdata);

	auto data = (T *)vdata.data;
	auto min_value = NumericStats::MinOrNull(stats);
	auto max_value = NumericStats::MaxOrNull(stats);
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto index = vdata.sel->get_index(idx);
		if (!vdata.validity.RowIsValid(index)) {
			continue;
		}
		if (!min_value.IsNull() && LessThan::Operation(data[index], min_value.GetValueUnsafe<T>())) { // LCOV_EXCL_START
			throw InternalException("Statistics mismatch: value is smaller than min.\nStatistics: %s\nVector: %s",
			                        stats.ToString(), vector.ToString(count));
		} // LCOV_EXCL_STOP
		if (!max_value.IsNull() && GreaterThan::Operation(data[index], max_value.GetValueUnsafe<T>())) {
			throw InternalException("Statistics mismatch: value is bigger than max.\nStatistics: %s\nVector: %s",
			                        stats.ToString(), vector.ToString(count));
		}
	}
}

void NumericStats::Verify(const BaseStatistics &stats, Vector &vector, const SelectionVector &sel, idx_t count) {
	auto &type = stats.GetType();
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		break;
	case PhysicalType::INT8:
		TemplatedVerify<int8_t>(stats, vector, sel, count);
		break;
	case PhysicalType::INT16:
		TemplatedVerify<int16_t>(stats, vector, sel, count);
		break;
	case PhysicalType::INT32:
		TemplatedVerify<int32_t>(stats, vector, sel, count);
		break;
	case PhysicalType::INT64:
		TemplatedVerify<int64_t>(stats, vector, sel, count);
		break;
	case PhysicalType::UINT8:
		TemplatedVerify<uint8_t>(stats, vector, sel, count);
		break;
	case PhysicalType::UINT16:
		TemplatedVerify<uint16_t>(stats, vector, sel, count);
		break;
	case PhysicalType::UINT32:
		TemplatedVerify<uint32_t>(stats, vector, sel, count);
		break;
	case PhysicalType::UINT64:
		TemplatedVerify<uint64_t>(stats, vector, sel, count);
		break;
	case PhysicalType::INT128:
		TemplatedVerify<hugeint_t>(stats, vector, sel, count);
		break;
	case PhysicalType::FLOAT:
		TemplatedVerify<float>(stats, vector, sel, count);
		break;
	case PhysicalType::DOUBLE:
		TemplatedVerify<double>(stats, vector, sel, count);
		break;
	default:
		throw InternalException("Unsupported type %s for numeric statistics verify", type.ToString());
	}
}

template <>
int8_t &NumericValueUnion::GetReferenceUnsafe() {
	return value_.tinyint;
}

template <>
int16_t &NumericValueUnion::GetReferenceUnsafe() {
	return value_.smallint;
}

template <>
int32_t &NumericValueUnion::GetReferenceUnsafe() {
	return value_.integer;
}

template <>
int64_t &NumericValueUnion::GetReferenceUnsafe() {
	return value_.bigint;
}

template <>
hugeint_t &NumericValueUnion::GetReferenceUnsafe() {
	return value_.hugeint;
}

template <>
uint8_t &NumericValueUnion::GetReferenceUnsafe() {
	return value_.utinyint;
}

template <>
uint16_t &NumericValueUnion::GetReferenceUnsafe() {
	return value_.usmallint;
}

template <>
uint32_t &NumericValueUnion::GetReferenceUnsafe() {
	return value_.uinteger;
}

template <>
uint64_t &NumericValueUnion::GetReferenceUnsafe() {
	return value_.ubigint;
}

template <>
float &NumericValueUnion::GetReferenceUnsafe() {
	return value_.float_;
}

template <>
double &NumericValueUnion::GetReferenceUnsafe() {
	return value_.double_;
}

} // namespace duckdb
