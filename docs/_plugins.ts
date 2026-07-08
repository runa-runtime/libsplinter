import basePath from "lume/plugins/base_path.ts";
import metas from "lume/plugins/metas.ts";
import { Options as SitemapOptions } from "lume/plugins/sitemap.ts";
import { favicon, Options as FaviconOptions } from "lume/plugins/favicon.ts";
import { merge } from "lume/core/utils/object.ts";
import robots from "lume/plugins/robots.ts";

import "lume/types.ts";

export interface Options {
  sitemap?: Partial<SitemapOptions>;
  favicon?: Partial<FaviconOptions>;
}

export const defaults: Options = {
  favicon: {
    input: "uploads/favicon.svg",
  },
};

/** Configure the site */
export default function (userOptions?: Options) {
  const options = merge(defaults, userOptions);

  return (site: Lume.Site) => {
    site
      .use(basePath())
      .use(metas())
      .use(robots({
        rules: [
          {
            userAgent: "*",
            allow: "/",
          },
        ],
      }))
      .add("splinter_thesis.pdf")
      .add("splinter-brand.css")
      .add("design")
      .use(favicon(options.favicon));
  };
}
