"""Generator semantics: validation is pure and every aggregate owns Elements."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "inspector_generator", Path(__file__).parents[1] / "codegen/inspector_codegen.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


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
        data = {"types": [self.integer, self.vector, self.array, self.tuple], "declarations": []}
        self.generator = module.Generator(data, {})

    def test_validation_does_not_generate_or_mutate(self):
        generator = self.generator
        for properties in ({"min": "2", "max": "1"}, {"step": "0"}, {"max": "2147483648"},
                           {"speed": "nan"}, {"widget": "slider"}):
            with self.assertRaises(ValueError):
                generator.validate_field(self.integer, properties)
            self.assertEqual(generator.counter, 0)
            self.assertEqual(generator.functions, [])
        original = {"widget": "readonly"}
        result = generator.validate_field(self.integer, original)
        self.assertEqual(original, {"widget": "readonly"})
        self.assertEqual(result["readonly"], "true")
        self.assertEqual(generator.functions, [])

    def test_nested_fixed_children_use_persistent_factories(self):
        generator = self.generator
        generator.element_factory("Component", self.tuple["id"])
        code = "\n".join(generator.functions)
        self.assertIn("std::get<1>(*value)", code)
        self.assertIn("(*value)[1]", code)
        self.assertIn("TSequenceFieldElement<", code)
        self.assertIn("TFieldElement<InspectorFields, Component, int", code)
        self.assertIn("std::make_unique<FieldGroup>", code)
        self.assertNotIn("TCompositeFieldElement<", code)
        self.assertNotIn("state.input", code)
        self.assertNotIn("containerPage", code)

    def test_custom_element_is_a_persistent_control(self):
        value = {"id": "MyValue", "__kind": "RecordType"}
        generator = module.Generator({"types": [value], "declarations": []},
                                     {"custom_elements": ["MyValue=MyControl"]})
        generator.element_factory("Component", "MyValue")
        code = "\n".join(generator.functions)
        self.assertIn("TFieldElement<InspectorFields, Component, MyValue, Access, MyControl>", code)
        self.assertIn("std::move(access), interaction, status", code)
        self.assertNotIn("TCompositeFieldElement", code)
        self.assertNotIn("TInspectorControl", code)

    def test_generated_factory_terminates_on_allocation_failure(self):
        component = {
            "id": "Component", "name": "Component", "fq_name": "Component",
            "__kind": "CXXRecordDecl", "attributes": ["component=true,editor=true"], "field_decls": []
        }
        generator = module.Generator({"types": [], "declarations": [component]}, {"name": "Component"})
        _, code = generator.component("Component")
        self.assertIn("catch (const std::bad_alloc&) { std::terminate(); }", code)
        self.assertNotIn("EEditorError::CAPACITY", code)


if __name__ == "__main__":
    unittest.main()
