# Vendored security backports

- grid 0.18.0: `Grid::expand_rows` uses checked arithmetic and panics on impossible allocation sizes, backporting becheran/grid commit `be213bd3528727148bef2d523c89e95d1fd9c072` (GHSA-38c5-483c-4qqp). This preserves the API required by gpui 0.2.2.

