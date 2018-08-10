
#include "execution/operator/physical_hash_aggregate.hpp"
#include "common/types/vector_operations.hpp"
#include "execution/expression_executor.hpp"

#include "parser/expression/aggregate_expression.hpp"

using namespace duckdb;
using namespace std;

PhysicalHashAggregate::PhysicalHashAggregate(
    vector<unique_ptr<AbstractExpression>> expressions)
    : PhysicalAggregate(move(expressions),
                        PhysicalOperatorType::HASH_GROUP_BY) {
	Initialize();
}

PhysicalHashAggregate::PhysicalHashAggregate(
    vector<unique_ptr<AbstractExpression>> expressions,
    vector<unique_ptr<AbstractExpression>> groups)
    : PhysicalAggregate(move(expressions), move(groups),
                        PhysicalOperatorType::HASH_GROUP_BY) {
	Initialize();
}

void PhysicalHashAggregate::Initialize() {}

void PhysicalHashAggregate::GetChunk(DataChunk &chunk,
                                     PhysicalOperatorState *state_) {
	auto state = reinterpret_cast<PhysicalHashAggregateOperatorState *>(state_);
	chunk.Reset();

	if (state->finished) {
		return;
	}

	ExpressionExecutor executor(state);

	do {
		if (children.size() > 0) {
			// resolve the child chunk if there is one
			children[0]->GetChunk(state->child_chunk, state->child_state.get());
			if (state->child_chunk.count == 0) {
				break;
			}
		}

		if (groups.size() > 0) {
			// aggregation with groups
			DataChunk &group_chunk = state->group_chunk;
			DataChunk &payload_chunk = state->payload_chunk;
			for (size_t i = 0; i < groups.size(); i++) {
				auto &expr = groups[i];
				executor.Execute(expr.get(), *group_chunk.data[i]);
				group_chunk.count = group_chunk.data[i]->count;
			}
			size_t i = 0;
			for (auto &expr : aggregates) {
				if (expr->children.size() > 0) {
					auto &child = expr->children[0];
					executor.Execute(child.get(), *payload_chunk.data[i]);
					payload_chunk.count = payload_chunk.data[i]->count;
					i++;
				}
			}
			state->ht->AddChunk(group_chunk, payload_chunk);
		} else {
			// aggregation without groups
			// merge into the fixed list of aggregates

			if (state->aggregates.size() == 0) {
				// first run: just store the values
				state->aggregates.resize(aggregates.size());
				for (size_t i = 0; i < aggregates.size(); i++) {
					state->aggregates[i] = executor.Execute(*aggregates[i]);
				}
			} else {
				// subsequent runs: merge the aggregates
				for (size_t i = 0; i < aggregates.size(); i++) {
					executor.Merge(*aggregates[i], state->aggregates[i]);
				}
			}
		}
	} while (state->child_chunk.count > 0);

	if (groups.size() > 0) {
		state->group_chunk.Reset();
		state->aggregate_chunk.Reset();
		state->ht->Scan(state->ht_scan_position, state->group_chunk,
		                state->aggregate_chunk);
		if (state->aggregate_chunk.count == 0) {
			state->finished = true;
			return;
		}
	} else {
		state->finished = true;
	}
	// we finished the child chunk
	// actually compute the final projection list now
	for (size_t i = 0; i < select_list.size(); i++) {
		auto &expr = select_list[i];
		executor.Execute(expr.get(), *chunk.data[i]);
	}
	chunk.count = chunk.data[0]->count;
	for (size_t i = 0; i < chunk.column_count; i++) {
		if (chunk.count != chunk.data[i]->count) {
			throw Exception("Projection count mismatch!");
		}
	}
}

unique_ptr<PhysicalOperatorState>
PhysicalHashAggregate::GetOperatorState(ExpressionExecutor *parent) {
	auto state = make_unique<PhysicalHashAggregateOperatorState>(
	    this, children.size() == 0 ? nullptr : children[0].get(), parent);
	if (groups.size() > 0) {
		size_t group_width = 0, payload_width = 0;
		vector<TypeId> payload_types, aggregate_types;
		std::vector<ExpressionType> aggregate_kind;
		for (auto &expr : groups) {
			group_width += GetTypeIdSize(expr->return_type);
		}
		for (auto &expr : aggregates) {
			aggregate_kind.push_back(expr->type);
			if (expr->children.size() > 0) {
				auto &child = expr->children[0];
				payload_types.push_back(child->return_type);
				payload_width += GetTypeIdSize(child->return_type);
			}
		}
		state->payload_chunk.Initialize(payload_types);

		state->ht = make_unique<SuperLargeHashTable>(
		    1024, group_width, payload_width, aggregate_kind);
	}
	return move(state);
}
