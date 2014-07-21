/* This file is part of VoltDB.
 * Copyright (C) 2008-2014 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "orderbyexecutor.h"

#include "common/tabletuple.h"
#include "execution/ProgressMonitorProxy.h"
#include "plannodes/orderbynode.h"
#include "plannodes/limitnode.h"
#include "storage/temptable.h"
#include "storage/tableiterator.h"

#include <algorithm>
#include <vector>

using namespace std;

namespace voltdb {

bool
OrderByExecutor::p_init(AbstractPlanNode* abstract_node,
                        TempTableLimits* limits)
{
    VOLT_TRACE("init OrderBy Executor");

    OrderByPlanNode* node = dynamic_cast<OrderByPlanNode*>(abstract_node);
    assert(node);
    assert(node->getInputTables().size() == 1);

    assert(node->getChildren()[0] != NULL);

    // Our output table should look exactly like out input table
    setTempOutputLikeInputTable(limits);

    // pickup an inlined limit, if one exists
    limit_node = dynamic_cast<LimitPlanNode*>(node->getInlinePlanNode(PLAN_NODE_TYPE_LIMIT));

    return true;
}

class TupleComparer
{
public:
    TupleComparer(const vector<AbstractExpression*>& keys,
                  const vector<SortDirectionType>& dirs)
        : m_keys(keys), m_dirs(dirs), m_keyCount(keys.size())
    {
        assert(keys.size() == dirs.size());
    }

    bool operator()(TableTuple ta, TableTuple tb)
    {
        for (size_t i = 0; i < m_keyCount; ++i)
        {
            AbstractExpression* k = m_keys[i];
            SortDirectionType dir = m_dirs[i];
            int cmp = k->eval(&ta, NULL).compare(k->eval(&tb, NULL));
            if (dir == SORT_DIRECTION_TYPE_ASC)
            {
                if (cmp < 0) return true;
                if (cmp > 0) return false;
            }
            else if (dir == SORT_DIRECTION_TYPE_DESC)
            {
                if (cmp < 0) return false;
                if (cmp > 0) return true;
            }
            else
            {
                throw SerializableEEException(VOLT_EE_EXCEPTION_TYPE_EEEXCEPTION,
                                              "Attempted to sort using"
                                              " SORT_DIRECTION_TYPE_INVALID");
            }
        }
        return false; // ta == tb on these keys
    }

private:
    const vector<AbstractExpression*>& m_keys;
    const vector<SortDirectionType>& m_dirs;
    size_t m_keyCount;
};

bool
OrderByExecutor::p_execute(const NValueArray &params)
{
    OrderByPlanNode* node = dynamic_cast<OrderByPlanNode*>(m_abstractNode);
    assert(node);
    TempTable* output_table = dynamic_cast<TempTable*>(node->getOutputTable());
    assert(output_table);
    Table* input_table = node->getInputTable();
    assert(input_table);

    //
    // OPTIMIZATION: NESTED LIMIT
    // How nice! We can also cut off our scanning with a nested limit!
    //
    int limit = -1;
    int offset = -1;
    if (limit_node != NULL)
    {
        limit_node->getLimitAndOffsetByReference(params, limit, offset);
    }

    VOLT_TRACE("Running OrderBy '%s'", m_abstractNode->debug().c_str());
    VOLT_TRACE("Input Table:\n '%s'", input_table->debug().c_str());
    TableIterator iterator = input_table->iterator();
    TableTuple tuple(input_table->schema());
    vector<TableTuple> xs;
    ProgressMonitorProxy pmp(m_engine, this);
    while (iterator.next(tuple))
    {
        pmp.countdownProgress();
        assert(tuple.isActive());
        xs.push_back(tuple);
    }
    VOLT_TRACE("\n***** Input Table PreSort:\n '%s'",
               input_table->debug().c_str());


    if (limit >= 0 && xs.begin() + limit + offset < xs.end()) {
        // partial sort
        partial_sort(xs.begin(), xs.begin() + limit + offset, xs.end(),
                TupleComparer(node->getSortExpressions(), node->getSortDirections()));
    } else {
        // full sort
        sort(xs.begin(), xs.end(),
                TupleComparer(node->getSortExpressions(), node->getSortDirections()));
    }

    int tuple_ctr = 0;
    int tuple_skipped = 0;
    for (vector<TableTuple>::iterator it = xs.begin(); it != xs.end(); it++)
    {
        //
        // Check if has gone past the offset
        //
        if (tuple_skipped < offset) {
            tuple_skipped++;
            continue;
        }

        VOLT_TRACE("\n***** Input Table PostSort:\n '%s'",
                   input_table->debug().c_str());
        output_table->insertTupleNonVirtual(*it);
        pmp.countdownProgress();
        //
        // Check whether we have gone past our limit
        //
        if (limit >= 0 && ++tuple_ctr >= limit) {
            break;
        }
    }
    VOLT_TRACE("Result of OrderBy:\n '%s'", output_table->debug().c_str());

    cleanupInputTempTable(input_table);

    return true;
}

OrderByExecutor::~OrderByExecutor() {
}

} // namespace voltdb
