"""Small formatting-tolerant helpers for source-level architecture checks."""


def compact_source(source: str) -> str:
    """Remove formatting whitespace before checking a source invariant.

    Callers should pass the expected snippet through this helper as well.  The
    checks remain ordered source-token checks; this only makes them insensitive
    to the formatter's spaces and line breaks.  Quoted literals and comments
    are copied verbatim so their user-visible text remains meaningful.
    """

    compacted: list[str] = []
    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index)
            if end < 0:
                compacted.append(source[index:])
                break
            compacted.append(source[index:end])
            index = end
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            if end < 0:
                compacted.append(source[index:])
                break
            end += 2
            compacted.append(source[index:end])
            index = end
            continue
        if source[index] in {"\"", "'"}:
            quote = source[index]
            end = index + 1
            while end < len(source):
                if source[end] == "\\":
                    end += 2
                    continue
                if source[end] == quote:
                    end += 1
                    break
                end += 1
            compacted.append(source[index:end])
            index = end
            continue
        if source[index].isspace():
            index += 1
            continue
        compacted.append(source[index])
        index += 1
    return "".join(compacted)
