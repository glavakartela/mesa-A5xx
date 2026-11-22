# Patch series

Base: `649a04b` ("Import Mesa source snapshot e9b12c27").
`0001..0009` are the current a5xx bring-up commits up to `43823af`.
`a5xx-all.patch` is the same change as one diff.

The patches are generated with:

```sh
git format-patch 649a04b..master -o a5xx/patches \
  -- . ':(exclude)a5xx/patches/*'
git diff 649a04b..master -- . ':(exclude)a5xx/patches/*' \
  > a5xx/patches/a5xx-all.patch
```

The commits are already in this repository; the patches are provided for
portability/review.

```sh
git am a5xx/patches/00*.patch
```
