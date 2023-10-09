#include "modules/api.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void modify_roi_in(
    dt_graph_t *graph,
    dt_module_t *module)
{
  dt_roi_t *ri = &module->connector[0].roi;
  ri->wd = ri->full_wd;
  ri->ht = ri->full_ht;
  ri->scale = 1.0f;
}

void modify_roi_out(
    dt_graph_t *graph,
    dt_module_t *module)
{ // always give full size and negotiate half or not in modify_roi_in
  dt_roi_t *ri = &module->connector[0].roi;
  dt_roi_t *ro = &module->connector[1].roi;
  ro->full_wd = ri->full_wd;
  ro->full_ht = ri->full_ht;
}

void
create_nodes(
    dt_graph_t  *graph,
    dt_module_t *module)
{
  const dt_image_params_t *img_param = dt_module_get_input_img_param(graph, module, dt_token("input"));
  if(!img_param) return; // must have disconnected input somewhere
  const int block = img_param->filters == 9u ? 3 : 2;
  module->img_param.filters = 0u; // after we're done there won't be any more mosaic
  const int wd = module->connector[0].roi.wd, ht = module->connector[0].roi.ht;
  dt_roi_t roi_full = module->connector[0].roi;
  dt_roi_t roi_half = module->connector[0].roi;
  roi_half.full_wd /= block;
  roi_half.full_ht /= block;
  roi_half.wd /= block;
  roi_half.ht /= block;
  const int pc[] = { img_param->filters };
  if(module->connector[1].roi.scale >= block)
  { // half size
    const int id_half = dt_node_add(graph, module, "demosaic", "halfsize",
        roi_half.wd, roi_half.ht, 1, sizeof(pc), pc, 2,
        "input",  "read",  "rggb", "*",   -1ul,
        "output", "write", "rgba", "f16", &roi_half);
    dt_connector_copy(graph, module, 0, id_half, 0);
    if(block != module->connector[1].roi.scale)
    { // resample to get to the rest of the resolution, only if block != scale!
      const int id_resample = dt_node_add(graph, module, "shared", "resample",
          module->connector[1].roi.wd, module->connector[1].roi.ht, 1, 0, 0, 2,
          "input",  "read",  "rgba", "f16", -1ul,
          "output", "write", "rgba", "f16", &module->connector[1].roi);
      CONN(dt_node_connect(graph, id_half, 1, id_resample, 0));
      dt_connector_copy(graph, module, 1, id_resample, 1);
    }
    else dt_connector_copy(graph, module, 1, id_half, 1);
    return;
  }

  // else: full size

  if(1)
  { // bayer with RCD
    dt_roi_t hr = module->connector[0].roi;
    hr.wd /= 2;
    const int id_conv = dt_node_add(graph, module, "demosaic", "rcd_conv",
        module->connector[0].roi.wd, module->connector[0].roi.ht, 1, 0, 0, 4,
        "cfa", "read",  "*", "*",   -1ul,
        // XXX TODO: u8!
        "vh",  "write", "r", "f16",  &module->connector[0].roi,
        "pq",  "write", "r", "f16",  &hr,
        "lp",  "write", "r", "f16", &hr);
    const uint32_t tile_size_x = DT_RCD_TILE_SIZE_X - 2*DT_RCD_BORDER;
    const uint32_t tile_size_y = DT_RCD_TILE_SIZE_Y - 2*DT_RCD_BORDER;
    // how many tiles will we process?
    const uint32_t num_tiles_x = (wd + tile_size_x - 1)/tile_size_x;
    const uint32_t num_tiles_y = (ht + tile_size_y - 1)/tile_size_y;
    const uint32_t invx = num_tiles_x * DT_LOCAL_SIZE_X;
    const uint32_t invy = num_tiles_y * DT_LOCAL_SIZE_Y;
    const int id_fill = dt_node_add(graph, module, "demosaic", "rcd_fill",
        invx, invy, 1, 0, 0, 5,
        "cfa", "read", "*", "*", -1ul,
        "vh",  "read", "*", "*", -1ul,
        "pq",  "read", "*", "*", -1ul,
        "lp",  "read", "*", "*", -1ul,
        "output", "write", "rgba", "f16", &module->connector[0].roi);
    CONN(dt_node_connect_named(graph, id_conv, "vh", id_fill, "vh"));
    CONN(dt_node_connect_named(graph, id_conv, "pq", id_fill, "pq"));
    CONN(dt_node_connect_named(graph, id_conv, "lp", id_fill, "lp"));
    dt_connector_copy(graph, module, 0, id_conv, 0);
    dt_connector_copy(graph, module, 0, id_fill, 0);
    if(module->connector[1].roi.scale != 1.0)
    { // add resample node to graph, copy its output instead:
      const int id_resample = dt_node_add(graph, module, "shared", "resample",
          module->connector[1].roi.wd, module->connector[1].roi.ht, 1, 0, 0, 2,
          "input",  "read",  "rgba", "f16", -1ul,
          "output", "write", "rgba", "f16", &module->connector[1].roi);
      CONN(dt_node_connect(graph, id_fill, 4, id_resample, 0));
      dt_connector_copy(graph, module, 1, id_resample, 1);
    }
    else dt_connector_copy(graph, module, 1, id_fill, 4);
    return;
  }

  // bayer or xtrans with gaussian splats
  const int id_down = dt_node_add(graph, module, "demosaic", "down",
      wd/block, ht/block, 1, sizeof(pc), pc, 2,
      "input", "read", "rggb", "*", -1ul,
      "output", "write", "y", "f16", &roi_half);
  const int id_gauss = dt_node_add(graph, module, "demosaic", "gauss",
      wd/block, ht/block, 1, sizeof(pc), pc, 3,
      "input",  "read",  "y",    "f16", -1ul,
      "orig",   "read",  "rggb", "*",   -1ul,
      "output", "write", "rgba", "f16", &roi_half);
  CONN(dt_node_connect(graph, id_down, 1, id_gauss, 0));
  dt_connector_copy(graph, module, 0, id_gauss, 1);

  const int id_splat = dt_node_add(graph, module, "demosaic", "splat",
      wd, ht, 1, sizeof(pc), pc, 3,
      "input",  "read",  "rggb", "*",   -1ul,
      "gauss",  "read",  "rgba", "f16", -1ul,
      "output", "write", "g",    "f16", &roi_full);
  dt_connector_copy(graph, module, 0, id_splat, 0);
  CONN(dt_node_connect(graph, id_gauss, 2, id_splat, 1));
  dt_connector_copy(graph, module, 0, id_down,  0);
  // dt_connector_copy(graph, module, 1, id_gauss, 2); // XXX DEBUG see output of gaussian params
  // return;

  // fix colour casts
  const int id_fix = dt_node_add(graph, module, "demosaic", "fix",
      wd, ht, 1, sizeof(pc), pc, 4,
      "input",  "read",  "rggb", "*",   -1ul,
      "green",  "read",  "g",    "*",   -1ul,
      "cov",    "read",  "rgba", "f16", -1ul,
      "output", "write", "rgba", "f16", &roi_full);
  dt_connector_copy(graph, module, 0, id_fix, 0);
  CONN(dt_node_connect(graph, id_splat, 2, id_fix, 1));
  CONN(dt_node_connect(graph, id_gauss, 2, id_fix, 2));

  if(module->connector[1].roi.scale != 1.0)
  { // add resample node to graph, copy its output instead:
    const int id_resample = dt_node_add(graph, module, "shared", "resample",
        module->connector[1].roi.wd, module->connector[1].roi.ht, 1, 0, 0, 2,
        "input",  "read",  "rgba", "f16", -1ul,
        "output", "write", "rgba", "f16", &module->connector[1].roi);
    CONN(dt_node_connect(graph, id_fix, 3, id_resample, 0));
    dt_connector_copy(graph, module, 1, id_resample, 1);
  }
  else dt_connector_copy(graph, module, 1, id_fix, 3);
}
