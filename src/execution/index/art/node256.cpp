#include "duckdb/execution/index/art/node256.hpp"

#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/art_node.hpp"
#include "duckdb/execution/index/art/node48.hpp"

namespace duckdb {

void Node256::Free(ART &art, ARTNode &node) {

	D_ASSERT(node);
	D_ASSERT(!node.IsSwizzled());

	auto n256 = node.Get<Node256>(art);

	// free all children
	if (n256->count) {
		for (idx_t i = 0; i < ARTNode::NODE_256_CAPACITY; i++) {
			if (n256->children[i]) {
				ARTNode::Free(art, n256->children[i]);
			}
		}
	}

	art.DecreaseMemorySize(sizeof(Node256));
}

Node256 *Node256::Initialize(ART &art, const ARTNode &node) {

	auto n256 = node.Get<Node256>(art);
	art.IncreaseMemorySize(sizeof(Node256));

	n256->count = 0;
	n256->prefix.Initialize();
	for (idx_t i = 0; i < ARTNode::NODE_256_CAPACITY; i++) {
		n256->children[i] = ARTNode();
	}
	return n256;
}

void Node256::Vacuum(ART &art, const unordered_set<ARTNodeType, ARTNodeTypeHash> &vacuum_nodes) {

	for (idx_t i = 0; i < ARTNode::NODE_256_CAPACITY; i++) {
		if (children[i]) {
			ARTNode::Vacuum(art, children[i], vacuum_nodes);
		}
	}
}

void Node256::InitializeMerge(ART &art, unordered_map<ARTNodeType, idx_t, ARTNodeTypeHash> &buffer_counts) {

	for (idx_t i = 0; i < ARTNode::NODE_256_CAPACITY; i++) {
		if (children[i]) {
			children[i].InitializeMerge(art, buffer_counts);
		}
	}
}

void Node256::InsertChild(ART &art, ARTNode &node, const uint8_t &byte, ARTNode &child) {

	D_ASSERT(node);
	D_ASSERT(!node.IsSwizzled());
	auto n256 = node.Get<Node256>(art);

	n256->count++;
	n256->children[byte] = child;
}

void Node256::DeleteChild(ART &art, ARTNode &node, idx_t pos) {

	D_ASSERT(node);
	D_ASSERT(!node.IsSwizzled());
	auto n256 = node.Get<Node256>(art);

	// free the child and decrease the count
	ARTNode::Free(art, n256->children[pos]);
	n256->count--;

	// shrink node to Node48
	if (n256->count <= ARTNode::NODE_256_SHRINK_THRESHOLD) {

		auto new_n48_node = ARTNode::New(art, ARTNodeType::NODE_48);
		auto new_n48 = Node48::Initialize(art, new_n48_node);

		new_n48->prefix.Move(n256->prefix);

		for (idx_t i = 0; i < ARTNode::NODE_256_CAPACITY; i++) {
			if (n256->children[i]) {
				new_n48->child_index[i] = new_n48->count;
				new_n48->children[new_n48->count++] = n256->children[i];
				n256->children[i] = ARTNode();
			}
		}

		n256->count = 0;
		ARTNode::Free(art, node);
		node = new_n48_node;
	}
}

void Node256::ReplaceChild(const idx_t &pos, ARTNode &child) {
	D_ASSERT(pos < ARTNode::NODE_256_CAPACITY);
	children[pos] = child;
}

ARTNode *Node256::GetChild(const idx_t &pos) {
	D_ASSERT(pos < ARTNode::NODE_256_CAPACITY);
	return &children[pos];
}

uint8_t Node256::GetKeyByte(const idx_t &pos) const {
	D_ASSERT(pos < ARTNode::NODE_256_CAPACITY);
	return pos;
}

idx_t Node256::GetChildPos(const uint8_t &byte) const {
	if (children[byte]) {
		return byte;
	}
	return DConstants::INVALID_INDEX;
}

idx_t Node256::GetChildPosGreaterEqual(const uint8_t &byte, bool &inclusive) const {
	for (idx_t pos = byte; pos < ARTNode::NODE_256_CAPACITY; pos++) {
		if (children[pos]) {
			inclusive = false;
			if (pos == byte) {
				inclusive = true;
			}
			return pos;
		}
	}
	return DConstants::INVALID_INDEX;
}

idx_t Node256::GetMinPos() const {
	for (idx_t i = 0; i < ARTNode::NODE_256_CAPACITY; i++) {
		if (children[i]) {
			return i;
		}
	}
	return DConstants::INVALID_INDEX;
}

idx_t Node256::GetNextPos(idx_t pos) const {
	pos == DConstants::INVALID_INDEX ? pos = 0 : pos++;
	for (; pos < ARTNode::NODE_256_CAPACITY; pos++) {
		if (children[pos]) {
			return pos;
		}
	}
	return DConstants::INVALID_INDEX;
}

idx_t Node256::GetNextPosAndByte(idx_t pos, uint8_t &byte) const {
	pos == DConstants::INVALID_INDEX ? pos = 0 : pos++;
	for (; pos < ARTNode::NODE_256_CAPACITY; pos++) {
		if (children[pos]) {
			byte = uint8_t(pos);
			return pos;
		}
	}
	return DConstants::INVALID_INDEX;
}

BlockPointer Node256::Serialize(ART &art, MetaBlockWriter &writer) {

	// recurse into children and retrieve child block pointers
	vector<BlockPointer> child_block_pointers;
	for (idx_t i = 0; i < ARTNode::NODE_256_CAPACITY; i++) {
		child_block_pointers.push_back(children[i].Serialize(art, writer));
	}

	// get pointer and write fields
	auto block_pointer = writer.GetBlockPointer();
	writer.Write(ARTNodeType::NODE_256);
	writer.Write<uint16_t>(count);
	prefix.Serialize(art, writer);

	// write child block pointers
	for (auto &child_block_pointer : child_block_pointers) {
		writer.Write(child_block_pointer.block_id);
		writer.Write(child_block_pointer.offset);
	}

	return block_pointer;
}

void Node256::Deserialize(ART &art, MetaBlockReader &reader) {

	count = reader.Read<uint16_t>();
	prefix.Deserialize(art, reader);

	// read child block pointers
	for (idx_t i = 0; i < ARTNode::NODE_256_CAPACITY; i++) {
		children[i] = ARTNode(reader);
	}

	art.IncreaseMemorySize(sizeof(Node256));
}

} // namespace duckdb
