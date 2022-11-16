#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/common/sort/sort.hpp"
#include "duckdb/common/types/column_data_collection.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

struct SortedAggregateBindData : public FunctionData {
	SortedAggregateBindData(ClientContext &context, const AggregateFunction &function_p,
	                        vector<unique_ptr<Expression>> &children, unique_ptr<FunctionData> bind_info_p,
	                        const BoundOrderModifier &order_bys)
	    : buffer_manager(BufferManager::GetBufferManager(context)), function(function_p), bind_info(move(bind_info_p)) {
		arg_types.reserve(children.size());
		for (const auto &child : children) {
			arg_types.emplace_back(child->return_type);
		}
		sort_types.reserve(order_bys.orders.size());
		for (auto &order : order_bys.orders) {
			orders.emplace_back(order.Copy());
			sort_types.emplace_back(order.expression->return_type);
		}
	}

	SortedAggregateBindData(const SortedAggregateBindData &other)
	    : buffer_manager(other.buffer_manager), function(other.function), arg_types(other.arg_types),
	      sort_types(other.sort_types) {
		if (other.bind_info) {
			bind_info = other.bind_info->Copy();
		}
		for (auto &order : other.orders) {
			orders.emplace_back(order.Copy());
		}
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_unique<SortedAggregateBindData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = (const SortedAggregateBindData &)other_p;
		if (bind_info && other.bind_info) {
			if (!bind_info->Equals(*other.bind_info)) {
				return false;
			}
		} else if (bind_info || other.bind_info) {
			return false;
		}
		if (function != other.function) {
			return false;
		}
		if (orders.size() != other.orders.size()) {
			return false;
		}
		for (size_t i = 0; i < orders.size(); ++i) {
			if (!orders[i].Equals(other.orders[i])) {
				return false;
			}
		}
		return true;
	}

	BufferManager &buffer_manager;
	AggregateFunction function;
	vector<LogicalType> arg_types;
	unique_ptr<FunctionData> bind_info;
	vector<LogicalType> sort_types;
	vector<BoundOrderByNode> orders;
};

struct SortedAggregateState {
	static const idx_t BUFFER_CAPACITY = STANDARD_VECTOR_SIZE;

	SortedAggregateState() : nsel(0) {
	}

	unique_ptr<ColumnDataCollection> arguments;
	unique_ptr<ColumnDataCollection> ordering;

	DataChunk sort_buffer;
	DataChunk arg_buffer;

	void Flush(SortedAggregateBindData &order_bind) {
		if (ordering) {
			return;
		}

		ordering = make_unique<ColumnDataCollection>(order_bind.buffer_manager, order_bind.sort_types);
		ordering->Append(sort_buffer);

		arguments = make_unique<ColumnDataCollection>(order_bind.buffer_manager, order_bind.arg_types);
		arguments->Append(arg_buffer);
	}

	void Update(SortedAggregateBindData &order_bind, DataChunk &sort_chunk, DataChunk &arg_chunk) {
		// Lazy instantiation of the buffer chunks
		if (sort_chunk.ColumnCount() != sort_buffer.ColumnCount()) {
			sort_buffer.Initialize(Allocator::DefaultAllocator(), sort_chunk.GetTypes());
		}
		if (arg_chunk.ColumnCount() != arg_buffer.ColumnCount()) {
			arg_buffer.Initialize(Allocator::DefaultAllocator(), arg_chunk.GetTypes());
		}

		if (sort_chunk.size() + sort_buffer.size() > BUFFER_CAPACITY) {
			Flush(order_bind);
		}
		if (ordering) {
			ordering->Append(sort_chunk);
			arguments->Append(arg_chunk);
		} else {
			sort_buffer.Append(sort_chunk, true);
			arg_buffer.Append(arg_chunk, true);
		}
	}

	void Combine(SortedAggregateBindData &order_bind, SortedAggregateState &other) {
		if (other.ordering) {
			// Force CDC if the other hash it
			Flush(order_bind);
			ordering->Combine(*other.ordering);
			arguments->Combine(*other.arguments);
		} else if (other.sort_buffer.size()) {
			Update(order_bind, other.sort_buffer, other.arg_buffer);
		}
	}

	void Finalize(LocalSortState &local_sort) {
		if (ordering) {
			const auto chunk_count = ordering->ChunkCount();
			for (idx_t chunk_idx = 0; chunk_idx < chunk_count; ++chunk_idx) {
				ordering->FetchChunk(chunk_idx, sort_buffer);
				arguments->FetchChunk(chunk_idx, arg_buffer);
				local_sort.SinkChunk(sort_buffer, arg_buffer);
			}
			ordering->Reset();
			arguments->Reset();
		} else {
			local_sort.SinkChunk(sort_buffer, arg_buffer);
		}
	}

	// Selection for scattering
	SelectionVector sel;
	idx_t nsel;
};

struct SortedAggregateFunction {
	template <typename STATE>
	static void Initialize(STATE *state) {
		new (state) STATE();
	}

	template <typename STATE>
	static void Destroy(STATE *state) {
		state->~STATE();
	}

	static void ProjectInputs(Vector inputs[], SortedAggregateBindData *order_bind, idx_t input_count, idx_t count,
	                          DataChunk &arg_chunk, DataChunk &sort_chunk) {
		idx_t col = 0;

		arg_chunk.InitializeEmpty(order_bind->arg_types);
		for (auto &dst : arg_chunk.data) {
			dst.Reference(inputs[col++]);
		}
		arg_chunk.SetCardinality(count);

		sort_chunk.InitializeEmpty(order_bind->sort_types);
		for (auto &dst : sort_chunk.data) {
			dst.Reference(inputs[col++]);
		}
		sort_chunk.SetCardinality(count);
	}

	static void SimpleUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, data_ptr_t state,
	                         idx_t count) {
		const auto order_bind = (SortedAggregateBindData *)aggr_input_data.bind_data;
		DataChunk arg_chunk;
		DataChunk sort_chunk;
		ProjectInputs(inputs, order_bind, input_count, count, arg_chunk, sort_chunk);

		const auto order_state = (SortedAggregateState *)state;
		order_state->Update(*order_bind, sort_chunk, arg_chunk);
	}

	static void ScatterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &states,
	                          idx_t count) {
		if (!count) {
			return;
		}

		// Append the arguments to the two sub-collections
		const auto order_bind = (SortedAggregateBindData *)aggr_input_data.bind_data;
		DataChunk arg_inputs;
		DataChunk sort_inputs;
		ProjectInputs(inputs, order_bind, input_count, count, arg_inputs, sort_inputs);

		// We have to scatter the chunks one at a time
		// so build a selection vector for each one.
		UnifiedVectorFormat svdata;
		states.ToUnifiedFormat(count, svdata);

		// Build the selection vector for each state.
		auto sdata = (SortedAggregateState **)svdata.data;
		for (idx_t i = 0; i < count; ++i) {
			auto sidx = svdata.sel->get_index(i);
			auto order_state = sdata[sidx];
			if (!order_state->sel.data()) {
				order_state->sel.Initialize();
			}
			order_state->sel.set_index(order_state->nsel++, i);
		}

		// Append nonempty slices to the arguments
		for (idx_t i = 0; i < count; ++i) {
			auto sidx = svdata.sel->get_index(i);
			auto order_state = sdata[sidx];
			if (!order_state->nsel) {
				continue;
			}

			DataChunk arg_chunk;
			arg_chunk.InitializeEmpty(arg_inputs.GetTypes());
			arg_chunk.Slice(arg_inputs, order_state->sel, order_state->nsel);

			DataChunk sort_chunk;
			sort_chunk.InitializeEmpty(sort_inputs.GetTypes());
			sort_chunk.Slice(sort_inputs, order_state->sel, order_state->nsel);

			order_state->Update(*order_bind, sort_chunk, arg_chunk);

			// Mark the slice as empty now we have consumed it.
			order_state->nsel = 0;
		}
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE *target, AggregateInputData &aggr_input_data) {
		const auto order_bind = (SortedAggregateBindData *)aggr_input_data.bind_data;
		auto &other = const_cast<STATE &>(source);
		target->Combine(*order_bind, other);
	}

	static void Finalize(Vector &states, AggregateInputData &aggr_input_data, Vector &result, idx_t count,
	                     idx_t offset) {
		const auto order_bind = (SortedAggregateBindData *)aggr_input_data.bind_data;
		auto &buffer_manager = order_bind->buffer_manager;
		auto &orders = order_bind->orders;
		RowLayout payload_layout;
		payload_layout.Initialize(order_bind->arg_types);

		//	 Reusable inner state
		vector<data_t> agg_state(order_bind->function.state_size());
		Vector agg_state_vec(Value::POINTER((idx_t)agg_state.data()));

		// State variables
		const auto input_count = order_bind->function.arguments.size();
		auto bind_info = order_bind->bind_info.get();
		AggregateInputData aggr_bind_info(bind_info, Allocator::DefaultAllocator());

		// Inner aggregate APIs
		auto initialize = order_bind->function.initialize;
		auto destructor = order_bind->function.destructor;
		auto simple_update = order_bind->function.simple_update;
		auto update = order_bind->function.update;
		auto finalize = order_bind->function.finalize;

		auto sdata = FlatVector::GetData<SortedAggregateState *>(states);
		for (idx_t i = 0; i < count; ++i) {
			initialize(agg_state.data());
			auto state = sdata[i];

			// Apply the sort before delegating the chunks
			auto global_sort = make_unique<GlobalSortState>(buffer_manager, orders, payload_layout);
			LocalSortState local_sort;
			local_sort.Initialize(*global_sort, global_sort->buffer_manager);
			state->Finalize(local_sort);
			global_sort->AddLocalState(local_sort);

			if (!global_sort->sorted_blocks.empty()) {
				global_sort->PrepareMergePhase();
				while (global_sort->sorted_blocks.size() > 1) {
					global_sort->InitializeMergeRound();
					MergeSorter merge_sorter(*global_sort, global_sort->buffer_manager);
					merge_sorter.PerformInMergeRound();
					global_sort->CompleteMergeRound(false);
				}

				auto &chunk = state->arg_buffer;
				PayloadScanner scanner(*global_sort);
				for (;;) {
					chunk.Reset();
					scanner.Scan(chunk);
					if (chunk.size() == 0) {
						break;
					}
					// These are all simple updates, so use it if available
					if (simple_update) {
						simple_update(chunk.data.data(), aggr_bind_info, input_count, agg_state.data(), chunk.size());
					} else {
						// We are only updating a constant state
						agg_state_vec.SetVectorType(VectorType::CONSTANT_VECTOR);
						update(chunk.data.data(), aggr_bind_info, input_count, agg_state_vec, chunk.size());
					}
				}
			}

			// Finalize a single value at the next offset
			agg_state_vec.SetVectorType(states.GetVectorType());
			finalize(agg_state_vec, aggr_bind_info, result, 1, i + offset);

			if (destructor) {
				destructor(agg_state_vec, 1);
			}
		}
	}

	static void Serialize(FieldWriter &writer, const FunctionData *bind_data, const AggregateFunction &function) {
		throw NotImplementedException("FIXME: serialize sorted aggregate not supported");
	}
	static unique_ptr<FunctionData> Deserialize(ClientContext &context, FieldReader &reader,
	                                            AggregateFunction &function) {
		throw NotImplementedException("FIXME: deserialize sorted aggregate not supported");
	}
};

unique_ptr<FunctionData> FunctionBinder::BindSortedAggregate(AggregateFunction &bound_function,
                                                             vector<unique_ptr<Expression>> &children,
                                                             unique_ptr<FunctionData> bind_info,
                                                             unique_ptr<BoundOrderModifier> order_bys) {

	auto sorted_bind =
	    make_unique<SortedAggregateBindData>(context, bound_function, children, move(bind_info), *order_bys);

	// The arguments are the children plus the sort columns.
	for (auto &order : order_bys->orders) {
		children.emplace_back(move(order.expression));
	}

	vector<LogicalType> arguments;
	arguments.reserve(children.size());
	for (const auto &child : children) {
		arguments.emplace_back(child->return_type);
	}

	// Replace the aggregate with the wrapper
	AggregateFunction ordered_aggregate(
	    bound_function.name, arguments, bound_function.return_type, AggregateFunction::StateSize<SortedAggregateState>,
	    AggregateFunction::StateInitialize<SortedAggregateState, SortedAggregateFunction>,
	    SortedAggregateFunction::ScatterUpdate,
	    AggregateFunction::StateCombine<SortedAggregateState, SortedAggregateFunction>,
	    SortedAggregateFunction::Finalize, SortedAggregateFunction::SimpleUpdate, nullptr,
	    AggregateFunction::StateDestroy<SortedAggregateState, SortedAggregateFunction>);
	ordered_aggregate.serialize = SortedAggregateFunction::Serialize;
	ordered_aggregate.deserialize = SortedAggregateFunction::Deserialize;
	ordered_aggregate.null_handling = bound_function.null_handling;

	bound_function = move(ordered_aggregate);

	return move(sorted_bind);
}

} // namespace duckdb
