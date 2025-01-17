// Copyright (c) 2021 CINN Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <algorithm>

#include "paddle/cinn/common/context.h"

#include "paddle/cinn/hlir/dialect/operator/ir/symbol_bindings.h"
#include "paddle/cinn/hlir/framework/node.h"
#include "paddle/common/enforce.h"
namespace cinn {
namespace hlir {
namespace framework {

std::tuple<cinn::common::GraphEdge*, cinn::common::GraphEdge*> Node::LinkTo(
    NodeData* other) {
  return this->cinn::common::GraphNode::LinkTo(
      other->as<cinn::common::GraphNode>());
}

std::tuple<cinn::common::GraphEdge*, cinn::common::GraphEdge*> NodeData::LinkTo(
    Node* other) {
  return this->cinn::common::GraphNode::LinkTo(
      other->as<cinn::common::GraphNode>());
}

void Node::Controls(NodeData* other) {
  return this->cinn::common::GraphNode::Controls(
      other->as<cinn::common::GraphNode>());
}

void NodeData::Controls(Node* other) {
  return this->cinn::common::GraphNode::Controls(
      other->as<cinn::common::GraphNode>());
}

namespace {

struct PyBindNodeAttrVisitor {
  std::stringstream& out;
  explicit PyBindNodeAttrVisitor(std::stringstream& out) : out(out) {}

  void operator()(int v) { out << "int: " << v; }
  void operator()(int64_t v) { out << "int64_t: " << v; }
  void operator()(float v) { out << "float: " << v; }
  void operator()(double v) { out << "double: " << v; }
  void operator()(bool v) { out << "bool: " << v; }
  void operator()(const std::string& v) { out << "string: " << v; }
#define VISIT_ELEMENTS(T__)                                      \
  void operator()(const std::vector<T__>& vs) {                  \
    if (vs.empty()) return;                                      \
    for (int i = 0; i < vs.size() - 1; i++) out << vs[i] << ","; \
    out << vs.back();                                            \
  }
  VISIT_ELEMENTS(int)
  VISIT_ELEMENTS(int64_t)
  VISIT_ELEMENTS(float)
  VISIT_ELEMENTS(double)
  VISIT_ELEMENTS(bool)
  VISIT_ELEMENTS(std::string)
  VISIT_ELEMENTS(symbol::DimExpr)
  VISIT_ELEMENTS(cinn::dialect::SymbolBinding)
};

}  // namespace

std::ostream& operator<<(std::ostream& os, const NodeAttr& node_attr) {
  std::stringstream ss;
  ss << "NodeAttr:\n";
  for (auto& item : node_attr.attr_store) {
    std::stringstream os;
    PyBindNodeAttrVisitor visitor(os);
    absl::visit(visitor, item.second);
    ss << "- " << os.str() << "\n";
  }
  os << ss.str();
  return os;
}

//! Using index to sort the input/output tensors
bool edge_index_compare(
    const cinn::common::Shared<cinn::common::GraphEdge>& a,
    const cinn::common::Shared<cinn::common::GraphEdge>& b) {
  PADDLE_ENFORCE_NOT_NULL(
      a.get(),
      phi::errors::InvalidArgument("The input edge should not be nullptr."));
  PADDLE_ENFORCE_NOT_NULL(
      b.get(),
      phi::errors::InvalidArgument("The input edge should not be nullptr."));
  return a->index() < b->index();
}

std::vector<cinn::common::Shared<cinn::common::GraphEdge>>
Node::inlinks_in_order() const {
  std::vector<cinn::common::Shared<cinn::common::GraphEdge>> ordered_links;
  for (auto& in_edge : this->inlinks()) {
    ordered_links.push_back(in_edge);
    PADDLE_ENFORCE_GE(
        in_edge->index(),
        0,
        phi::errors::InvalidArgument(
            "The index of a node's inlinks should be >= 0! Now index is: %d. "
            "Please check.",
            in_edge->index()));
  }
  std::sort(ordered_links.begin(), ordered_links.end(), edge_index_compare);
  return ordered_links;
}

std::vector<cinn::common::Shared<cinn::common::GraphEdge>>
Node::outlinks_in_order() const {
  std::vector<cinn::common::Shared<cinn::common::GraphEdge>> ordered_links;
  for (auto& out_edge : this->outlinks()) {
    ordered_links.push_back(out_edge);
    PADDLE_ENFORCE_GE(
        out_edge->index(),
        0,
        phi::errors::InvalidArgument(
            "The index of a node's outlinks should be >= 0! Now index is: %d. "
            "Please check.",
            out_edge->index()));
  }
  std::sort(ordered_links.begin(), ordered_links.end(), edge_index_compare);
  return ordered_links;
}

NodeData* InsertGraphOpNodeAfter(cinn::common::Graph* graph,
                                 Node* insert_node,
                                 NodeData* input_nodedata,
                                 Node* out_node,
                                 int pos) {
  CHECK(graph);
  CHECK(insert_node);
  CHECK(input_nodedata);
  input_nodedata->Controls(insert_node);
  cinn::common::Shared<Node> node_ptr(insert_node);
  auto* out_nodedata = new NodeData(
      node_ptr, 0, 0, cinn::common::UniqName(insert_node->id() + "_out"));
  insert_node->Controls(out_nodedata);
  std::vector<cinn::common::GraphNode*> old_sources;
  auto input_links = out_node->inlinks_in_order();

  if (out_node) {
    for (auto& link : input_links) {
      auto* source = link->source();
      // unlink and relink afterwards to make sure the order
      source->UnLinkSingleTo(out_node);
      old_sources.push_back(source);
    }
    for (int i = 0; i < old_sources.size(); i++) {
      auto* source = old_sources[i];
      if (i == pos) {
        out_nodedata->LinkTo(out_node);
      } else {
        source->LinkTo(out_node);
      }
    }
  }

  graph->RegisterNode(insert_node->id(), insert_node);
  graph->RegisterNode(out_nodedata->id(), out_nodedata);
  return out_nodedata;
}

NodeData* InsertGraphOpNodeBefore(cinn::common::Graph* graph,
                                  Node* insert_node,
                                  Node* input_node,
                                  NodeData* dst_data,
                                  int pos) {
  CHECK(graph);
  CHECK(insert_node);
  CHECK(input_node);
  CHECK(dst_data);
  auto node_ptr = dst_data->source_node;
  auto* input_node_out = new NodeData(
      node_ptr, 0, 0, cinn::common::UniqName(input_node->id() + "_out"));
  std::vector<cinn::common::GraphNode*> old_sinks;
  const auto& old_outlinks = input_node->outlinks_in_order();
  for (auto& link : old_outlinks) {
    auto sink = link->sink();
    // unlink and relink afterwards to make sure the right outputs order
    input_node->UnLinkSingleTo(sink);
    old_sinks.push_back(sink);
  }
  input_node_out->Controls(insert_node);
  insert_node->Controls(dst_data);
  dst_data->source_node = cinn::common::Shared<Node>(insert_node);

  for (int i = 0; i < old_sinks.size(); i++) {
    if (i == pos) {
      input_node->LinkTo(input_node_out);
    } else {
      auto outdata = old_sinks[i]->safe_as<NodeData>();
      input_node->LinkTo(outdata);
    }
  }

  graph->RegisterNode(input_node_out->id(), input_node_out);
  graph->RegisterNode(insert_node->id(), insert_node);
  return input_node_out;
}

}  // namespace framework
}  // namespace hlir
}  // namespace cinn
