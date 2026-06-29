// #include "zinc.h"
//
// #include <clang-c/Index.h>
//
// enum CXChildVisitResult visitor(
//     CXCursor cursor,
//     CXCursor parent,
//     CXClientData client_data) {
//     CXString kind = clang_getCursorKindSpelling(clang_getCursorKind(cursor));
//     CXString spelling = clang_getCursorSpelling(cursor);
//
//     printf("%s: %s\n",
//            clang_getCString(kind),
//            clang_getCString(spelling));
//
//     clang_disposeString(kind);
//     clang_disposeString(spelling);
//
//     clang_visitChildren(cursor, visitor, client_data);
//
//     return CXChildVisit_Continue;
// }
//
// ZNode *convertHeaderToZNode(ZParser *parser, ZToken *import) {
//     CXIndex index = clang_createIndex(0, 0);
//
//     const char *args[] = {
//         "-Iinclude",
//         "-xc",
//         "-std=c17"
//     };
//
//     CXTranslationUnit tu = clang_parseTranslationUnit(
//         index,
//         import->str,
//         args,
//         3,
//         NULL,
//         0,
//         CXTranslationUnit_None);
//
//     if (!tu) {
//         error(parser->state, import, "Invalid header file");
//         return NULL;
//     }
//
//     CXCursor root = clang_getTranslationUnitCursor(tu);
//
//     clang_visitChildren(root, visitor, NULL);
//
//     clang_disposeTranslationUnit(tu);
//     clang_disposeIndex(index);
//
//     return NULL;
// }
