/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 * This file is for func json_query.
 */

#ifndef OCEANBASE_SRC_SQL_ENGINE_EXPR_OB_EXPR_JSON_QUERY_H
#define OCEANBASE_SRC_SQL_ENGINE_EXPR_OB_EXPR_JSON_QUERY_H

#include "sql/engine/expr/ob_expr_operator.h"
#include "lib/json_type/ob_json_tree.h"
#include "lib/json_type/ob_json_base.h"
#include "ob_json_param_type.h"

using namespace oceanbase::common;

namespace oceanbase
{

namespace sql
{


class
ObExprJsonQuery : public ObFuncExprOperator
{
public:
    explicit ObExprJsonQuery(common::ObIAllocator &alloc);
    virtual ~ObExprJsonQuery();
    virtual int calc_result_typeN(ObExprResType& type,
                              ObExprResType* types,
                              int64_t param_num,
                              common::ObExprTypeCtx& type_ctx)
                              const override;
    static int eval_json_query(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res);
    virtual int cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                    ObExpr &rt_expr) const override;
    virtual common::ObCastMode get_cast_mode() const { return CM_ERROR_ON_SCALE_OVER;}

private:
  static int get_dest_type(const ObExpr &expr,
                          int32_t &dst_len,
                          ObEvalCtx& ctx,
                          ObObjType &dest_type,
                          bool &is_cover_by_error);
  static bool try_set_error_val(common::ObIAllocator *allocator, ObEvalCtx &ctx,
                                const ObExpr &expr, ObDatum &res, int &ret,
                                uint8_t error_type, uint8_t mismatch_type,
                                ObObjType dst_type);
  static int set_result(ObObjType dst_type, int32_t dst_len, ObIJsonBase *jb_res,
                        common::ObIAllocator *allocator, ObEvalCtx &ctx,
                        const ObExpr &expr, ObDatum &res, uint8_t error_type, uint8_t ascii_type, uint8_t pretty_type = 0, uint8_t is_truncate = 0);



  static int get_clause_opt(const ObExpr &expr,
                            ObEvalCtx &ctx,
                            uint8_t index,
                            bool &is_cover_by_error,
                            uint8_t &type,
                            uint8_t size_para);
  /*
  oracle mode get json path to JsonBase in static_typing_engine
  @param[in]  expr       the input arguments
  @param[in]  ctx        the eval context
  @param[in]  allocator  the Allocator in context
  @param[in]  index      the input arguments index
  @param[out] j_path     the pointer to JsonPath
  @param[out] is_null    the flag for null situation
  @param[out] is_cover_by_error    the flag for whether need cover by error clause
  @return Returns OB_SUCCESS on success, error code otherwise.
  */
  static int get_ora_json_path(const ObExpr &expr, ObEvalCtx &ctx,
                          common::ObArenaAllocator &allocator, ObJsonPath*& j_path,
                          uint16_t index, bool &is_null, bool &is_cover_by_error,
                          ObDatum*& json_datum);

    /*
  oracle mode get json doc to JsonBase in static_typing_engine
  @param[in]  expr       the input arguments
  @param[in]  ctx        the eval context
  @param[in]  allocator  the Allocator in context
  @param[in]  index      the input arguments index
  @param[out] j_base     the pointer to JsonBase
  @param[out] j_in_type     the pointer to input type
  @param[out] is_null    the flag for null situation
  @param[out] is_cover_by_error    the flag for whether need cover by error clause
  @return Returns OB_SUCCESS on success, error code otherwise.
  */
  static int get_ora_json_doc(const ObExpr &expr, ObEvalCtx &ctx,
                          common::ObArenaAllocator &allocator,
                          uint16_t index, ObIJsonBase*& j_base,
                           ObObjType dst_type,
                          bool &is_null, bool &is_cover_by_error);

  static int get_empty_option(ObJsonBaseVector &hits, bool &is_cover_by_error, int8_t empty_type,
                    bool &is_null_result, bool &is_null_json_obj, bool &is_null_json_array);
  static int get_clause_pre_asc_sca_opt(const ObExpr &expr, ObEvalCtx &ctx,
                                        bool &is_cover_by_error, uint8_t &pretty_type,
                                        uint8_t &ascii_type, uint8_t &scalars_type);
  static int get_single_obj_wrapper(uint8_t wrapper_type, int &use_wrapper, ObJsonNodeType in_type, uint8_t scalars_type);
  static int get_multi_scalars_wrapper_type(uint8_t wrapper_type, int &use_wrapper,
                                            ObJsonBaseVector &hits, uint8_t scalars_type);
/* code from ob_expr_cast for cal_result_type */
  const static int32_t OB_LITERAL_MAX_INT_LEN = 21;

  DISALLOW_COPY_AND_ASSIGN(ObExprJsonQuery);

};

} // sql
} // oceanbase

#endif  //OCEANBASE_SRC_SQL_ENGINE_EXPR_OB_EXPR_JSON_QUERY_H