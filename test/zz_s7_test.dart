import 'package:qjs_ultra/qjs_ultra.dart';
import 'engine_test.dart' show resolveLibPath;

void main() {
  final libPath = resolveLibPath()!;
  final engine = QuickjsEngine.createWith(const JsEngineConfig(), libPath: libPath);
  engine.registerAsyncFunction('a1', (args) async => 1);
  engine.registerFunction('s1', (args) => 2);
  final g = engine.getGlobalProperty('a1');
  final s = engine.getGlobalProperty('s1');
  print('a1 type=${g.runtimeType} s1 type=${s.runtimeType}');
  print('a1 via typeof: ${engine.evaluate("typeof a1")}');
  engine.dispose();
}
