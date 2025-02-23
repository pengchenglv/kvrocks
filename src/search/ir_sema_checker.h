/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#pragma once

#include <map>
#include <memory>

#include "fmt/core.h"
#include "index_info.h"
#include "ir.h"
#include "search_encoding.h"
#include "storage/redis_metadata.h"

namespace kqir {

struct SemaChecker {
  const IndexMap &index_map;

  const IndexInfo *current_index = nullptr;

  explicit SemaChecker(const IndexMap &index_map) : index_map(index_map) {}

  Status Check(Node *node) {
    if (auto v = dynamic_cast<SearchExpr *>(node)) {
      auto index_name = v->index->name;
      if (auto iter = index_map.find(index_name); iter != index_map.end()) {
        current_index = &iter->second;
        v->index->info = current_index;

        GET_OR_RET(Check(v->select.get()));
        GET_OR_RET(Check(v->query_expr.get()));
        if (v->limit) GET_OR_RET(Check(v->limit.get()));
        if (v->sort_by) GET_OR_RET(Check(v->sort_by.get()));
      } else {
        return {Status::NotOK, fmt::format("index `{}` not found", index_name)};
      }
    } else if (auto v [[maybe_unused]] = dynamic_cast<LimitClause *>(node)) {
      return Status::OK();
    } else if (auto v = dynamic_cast<SortByClause *>(node)) {
      if (auto iter = current_index->fields.find(v->field->name); iter == current_index->fields.end()) {
        return {Status::NotOK, fmt::format("field `{}` not found in index `{}`", v->field->name, current_index->name)};
      } else if (!iter->second.IsSortable()) {
        return {Status::NotOK, fmt::format("field `{}` is not sortable", v->field->name)};
      } else {
        v->field->info = &iter->second;
      }
    } else if (auto v = dynamic_cast<AndExpr *>(node)) {
      for (const auto &n : v->inners) {
        GET_OR_RET(Check(n.get()));
      }
    } else if (auto v = dynamic_cast<OrExpr *>(node)) {
      for (const auto &n : v->inners) {
        GET_OR_RET(Check(n.get()));
      }
    } else if (auto v = dynamic_cast<NotExpr *>(node)) {
      GET_OR_RET(Check(v->inner.get()));
    } else if (auto v = dynamic_cast<TagContainExpr *>(node)) {
      if (auto iter = current_index->fields.find(v->field->name); iter == current_index->fields.end()) {
        return {Status::NotOK, fmt::format("field `{}` not found in index `{}`", v->field->name)};
      } else if (auto meta = dynamic_cast<redis::SearchTagFieldMetadata *>(iter->second.metadata.get()); !meta) {
        return {Status::NotOK, fmt::format("field `{}` is not a tag field", v->field->name)};
      } else {
        v->field->info = &iter->second;

        if (v->tag->val.empty()) {
          return {Status::NotOK, "tag cannot be an empty string"};
        }

        if (v->tag->val.find(meta->separator) != std::string::npos) {
          return {Status::NotOK, fmt::format("tag cannot contain the separator `{}`", meta->separator)};
        }
      }
    } else if (auto v = dynamic_cast<NumericCompareExpr *>(node)) {
      if (auto iter = current_index->fields.find(v->field->name); iter == current_index->fields.end()) {
        return {Status::NotOK, fmt::format("field `{}` not found in index `{}`", v->field->name, current_index->name)};
      } else if (!dynamic_cast<redis::SearchNumericFieldMetadata *>(iter->second.metadata.get())) {
        return {Status::NotOK, fmt::format("field `{}` is not a numeric field", v->field->name)};
      } else {
        v->field->info = &iter->second;
      }
    } else if (auto v = dynamic_cast<SelectClause *>(node)) {
      for (const auto &n : v->fields) {
        if (auto iter = current_index->fields.find(n->name); iter == current_index->fields.end()) {
          return {Status::NotOK, fmt::format("field `{}` not found in index `{}`", n->name, current_index->name)};
        } else {
          n->info = &iter->second;
        }
      }
    } else if (auto v [[maybe_unused]] = dynamic_cast<BoolLiteral *>(node)) {
      return Status::OK();
    } else {
      return {Status::NotOK, fmt::format("unexpected IR node type: {}", node->Name())};
    }

    return Status::OK();
  }
};

}  // namespace kqir
