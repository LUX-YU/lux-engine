"""Production host-tool semantics. The original assertions survive the emitter migration."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

GENERATOR, TEMPLATES = sys.argv[1:3]
del sys.argv[1:3]

class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.integer = {"id": "int", "__kind": "BuiltinType", "size": 4}
        self.vector = {"id": "std::vector<int>", "__kind": "RecordType", "template_name": "std::vector",
                       "template_arguments": [{"kind": "type", "type_id": "int"}]}
        self.array = {"id": "std::array<std::vector<int>, 2>", "__kind": "RecordType",
                      "template_name": "std::array", "template_arguments": [
                          {"kind": "type", "type_id": self.vector["id"]}, {"integral_value": 2}]}
        self.tuple = {"id": "std::tuple<int, std::array<std::vector<int>, 2>>", "__kind": "RecordType",
                      "template_name": "std::tuple", "template_arguments": [
                          {"kind": "type", "type_id": "int"}, {"kind": "type", "type_id": self.array["id"]}]}
        self.types = [self.integer, self.vector, self.array, self.tuple]

    def generate(self, tid="int", properties=None, config=None, types=None, success=True):
        props = properties or {}
        field = {"id": "field", "name": "value", "fq_name": "Component::value", "__kind": "FieldDecl",
                 "visibility": 1, "type_id": tid, "attributes": [",".join(k+"="+v for k,v in props.items())]}
        component = {"id": "Component", "name": "Component", "fq_name": "Component", "__kind": "CXXRecordDecl",
                     "attributes": ["component=true,editor=true"], "field_decls": ["field"]}
        data = {"types": self.types if types is None else types, "declarations": [component, field]}
        cfg = dict(name="Fixture", logical_path="Fixture.hpp", components=["Component"])
        cfg.update(config or {})
        with tempfile.TemporaryDirectory(prefix="Inspector 空格 ") as temporary:
            root = Path(temporary)
            (root/"input.json").write_text(json.dumps(data), encoding="utf-8")
            (root/"config.json").write_text(json.dumps(cfg), encoding="utf-8")
            before = (root/"input.json").read_bytes()
            result = subprocess.run([GENERATOR,str(root/"config.json"),str(root/"input.json"),TEMPLATES,str(root/"stage")],
                                    capture_output=True, text=True, encoding="utf-8")
            self.assertEqual((root/"input.json").read_bytes(), before)
            if not success:
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertFalse((root/"stage").exists(), "semantic failure published output")
                return result.stderr
            self.assertEqual(result.returncode, 0, result.stderr)
            code = "\n".join(p.read_text(encoding="utf-8") for p in (root/"stage").glob("*.cpp"))
            model = json.loads((root/"stage/semantics.json").read_text(encoding="utf-8"))
            return code, model

    def test_validation_does_not_generate_or_mutate(self):
        for properties in ({"min":"2","max":"1"}, {"step":"0"}, {"max":"2147483648"},
                           {"speed":"nan"}, {"widget":"slider"}):
            self.generate(properties=properties, success=False)
        original = {"widget":"readonly"}
        _, model = self.generate(properties=original)
        self.assertEqual(original,{"widget":"readonly"})
        self.assertTrue(model["components"][0]["fields"][0]["read_only"])

    def test_nested_fixed_children_use_persistent_factories(self):
        code, _ = self.generate(self.tuple["id"])
        self.assertIn("std::get<1>(*value)",code)
        self.assertIn("(*value)[1]",code)
        self.assertIn("TSequenceFieldElement<",code)
        self.assertIn("TFieldElement<InspectorFields, Component, int",code)
        self.assertIn("std::make_unique<FieldGroup>",code)
        self.assertNotIn("TCompositeFieldElement<",code)
        self.assertNotIn("state.input",code)
        self.assertNotIn("containerPage",code)

    def test_custom_element_is_a_persistent_control(self):
        code, _ = self.generate("MyValue",config={"custom_elements":["MyValue=MyControl"]},
                                types=[{"id":"MyValue","__kind":"RecordType"}])
        self.assertIn("TFieldElement<InspectorFields, Component, MyValue, Access, MyControl>",code)
        self.assertIn("std::move(access), interaction, status",code)
        self.assertNotIn("TCompositeFieldElement",code)
        self.assertNotIn("TInspectorControl",code)

    def test_generated_factory_terminates_on_allocation_failure(self):
        code, _ = self.generate()
        self.assertIn("catch (const std::bad_alloc&) { std::terminate(); }",code)
        self.assertNotIn("EEditorError::CAPACITY",code)
        self.assertIn("RunInspectorFields",code)
        self.assertIn("RunInspectorComponent binding_",code)
        self.assertIn("std::make_shared<Component>",code)

    def test_integer_edges_are_exact(self):
        unsigned = {"id":"unsigned long long","__kind":"BuiltinType","size":8}
        code, _ = self.generate(unsigned["id"],{"min":"18446744073709551614","max":"18446744073709551615"},types=[unsigned])
        self.assertIn("18446744073709551615ULL",code)
        self.generate(unsigned["id"],{"min":"18446744073709551615","max":"18446744073709551614"},types=[unsigned],success=False)
        self.generate(unsigned["id"],{"max":"18446744073709551616"},types=[unsigned],success=False)
        signed = {"id":"long long","__kind":"BuiltinType","size":8}
        code, _ = self.generate(signed["id"],{"min":"-9223372036854775808","max":"9223372036854775807"},types=[signed])
        self.assertIn("(-9223372036854775807LL - 1)",code)
        self.generate(signed["id"],{"max":"9223372036854775808"},types=[signed],success=False)
        self.generate(properties={"step":"1.5"},success=False)
        self.generate(properties={"step":"1e2"})
        self.generate(properties={"step":"1.0e-9223372036854775808"},success=False)
        self.generate(properties={"step":"1.0e2147483647"},success=False)
        self.generate(properties={"step":"-0"},success=False)
        self.generate("MyValue", {"min":"2", "max":"1"}, config={"custom_elements":["MyValue=MyControl"]},
                      types=[{"id":"MyValue","__kind":"RecordType"}], success=False)

    def test_missing_array_metadata_is_rejected(self):
        self.generate("int[2]",types=[self.integer,{"id":"int[2]","__kind":"UnsupportedType"}],success=False)
        self.generate("int[2]",types=[self.integer,{"id":"int[2]","__kind":"ArrayType","array_size":2,"element_type_id":"int"}])

    def test_invalid_widgets_and_recursive_ownership(self):
        self.generate(properties={"widget":"mystery"},success=False)
        self.assertIn("unknown field annotation", self.generate(properties={"speeed":"1"},success=False))
        self.generate(properties={"readonly":"perhaps"},success=False)
        self.generate(properties={"display_name":'"unfinished'},success=False)
        self.generate(properties={"speed":"inf"},success=False)
        recursive={"id":"Cycle","__kind":"RecordType","template_name":"std::vector","template_arguments":[{"kind":"type","type_id":"Cycle"}]}
        self.generate("Cycle",types=[recursive],success=False)

    def test_literal_escaping_and_immutable_source(self):
        label='label 中文, "quote" \\path\nline'
        code, model = self.generate(properties={"display_name":json.dumps(label,ensure_ascii=False)})
        self.assertEqual(model["components"][0]["fields"][0]["label"],label)
        self.assertIn('\\012',code)
        self.assertIn('\\"quote\\"',code)
        self.assertIn("namespace lux::editor::scene::generated",code)
        self.assertIn("namespace lux::editor::scene::run_generated",code)

if __name__=="__main__":
    unittest.main()
