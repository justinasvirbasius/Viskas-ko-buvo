#!/usr/bin/env python3
"""
radical_roots_lab.py

A complete square-root / radical-notation programming map.

Covers:
    - radical sign √
    - hidden square-root index 2
    - radicand extraction from simple notation
    - real principal square root
    - equation roots x^2 = a, giving ±√a when appropriate
    - nth roots
    - complex roots
    - integer radical simplification, e.g. √72 = 6√2
    - safe numeric parsing for simple arithmetic radicands
    - CLI demo and self-tests

This file uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import ast
import cmath
import math
import operator
import re
import sys
from dataclasses import dataclass
from fractions import Fraction
from typing import Any, Callable, Iterable, Literal

TAU = 2.0 * math.pi

Number = int | float | complex


# ---------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------

class RadicalError(ValueError):
    """Base error for radical/root operations."""


class DomainError(RadicalError):
    """Raised when a real-number operation is outside the real domain."""


class ParseRadicalError(RadicalError):
    """Raised when radical notation cannot be parsed."""


# ---------------------------------------------------------------------
# Data structures: punctuation / notation objects
# ---------------------------------------------------------------------

@dataclass(frozen=True)
class RadicalNotation:
    """
    Represents the notation:

        ⁿ√radicand
        sqrt[index](radicand)
        root(index, radicand)

    index:
        2 means square root. If not printed, it is conventionally hidden.

    radicand:
        The expression under the radical/vinculum.

    coefficient:
        Optional outside multiplier, e.g. 6√2 has coefficient 6.

    imaginary_unit:
        True means multiply by i, e.g. 6i√2.
    """

    radicand: str
    index: int = 2
    coefficient: int | Fraction = 1
    imaginary_unit: bool = False

    def __post_init__(self) -> None:
        if self.index < 2:
            raise ValueError("radical index must be >= 2")
        if not self.radicand:
            raise ValueError("radicand cannot be empty")

    @property
    def is_square_root(self) -> bool:
        return self.index == 2

    def render_unicode(self) -> str:
        coeff = _format_coefficient(self.coefficient)
        imag = "i" if self.imaginary_unit else ""
        if self.index == 2:
            return f"{coeff}{imag}√({self.radicand})"
        return f"{coeff}{imag}⁽{self.index}⁾√({self.radicand})"

    def render_ascii(self) -> str:
        coeff = _format_coefficient(self.coefficient)
        imag = "i" if self.imaginary_unit else ""
        if self.index == 2:
            return f"{coeff}{imag}sqrt({self.radicand})"
        return f"{coeff}{imag}root({self.index}, {self.radicand})"

    def render_latex(self) -> str:
        coeff = _format_coefficient(self.coefficient)
        imag = "i" if self.imaginary_unit else ""
        if self.index == 2:
            return rf"{coeff}{imag}\sqrt{{{self.radicand}}}"
        return rf"{coeff}{imag}\sqrt[{self.index}]{{{self.radicand}}}"


@dataclass(frozen=True)
class RadicalParts:
    """The punctuation parts of a radical expression."""

    radical_sign: str
    index: int
    radicand: str
    vinculum_meaning: str
    principal_root_rule: str


@dataclass(frozen=True)
class SimplifiedSquareRoot:
    """
    Integer square-root simplification.

    Examples:
        √72     -> coefficient=6, radicand=2, imaginary_unit=False
        √-72    -> coefficient=6, radicand=2, imaginary_unit=True
        √36     -> coefficient=6, radicand=1, imaginary_unit=False
    """

    original: int
    coefficient: int
    radicand: int
    imaginary_unit: bool = False

    def notation(self) -> RadicalNotation:
        return RadicalNotation(
            coefficient=self.coefficient,
            radicand=str(self.radicand),
            index=2,
            imaginary_unit=self.imaginary_unit,
        )

    def render_unicode(self) -> str:
        if self.original == 0:
            return "0"

        imag = "i" if self.imaginary_unit else ""

        if self.radicand == 1:
            return f"{self.coefficient}{imag}"

        coeff = "" if self.coefficient == 1 else str(self.coefficient)
        return f"{coeff}{imag}√{self.radicand}"

    def render_ascii(self) -> str:
        if self.original == 0:
            return "0"

        imag = "i" if self.imaginary_unit else ""

        if self.radicand == 1:
            return f"{self.coefficient}{imag}"

        coeff = "" if self.coefficient == 1 else str(self.coefficient)
        return f"{coeff}{imag}sqrt({self.radicand})"


@dataclass(frozen=True)
class RootAnalysis:
    """Complete analysis of root notation and root values."""

    notation: RadicalNotation
    radicand_value: Number
    principal_value: complex
    all_complex_roots: tuple[complex, ...]
    all_real_roots: tuple[float, ...]
    note: str


# ---------------------------------------------------------------------
# Formatting utilities
# ---------------------------------------------------------------------

def _format_coefficient(c: int | Fraction) -> str:
    if c == 1:
        return ""
    if c == -1:
        return "-"
    return str(c)


def format_complex(z: complex, *, digits: int = 12) -> str:
    """Human-friendly complex number rendering."""
    re_part = 0.0 if abs(z.real) < 10 ** (-digits + 2) else z.real
    im_part = 0.0 if abs(z.imag) < 10 ** (-digits + 2) else z.imag

    if im_part == 0.0:
        return f"{re_part:.{digits}g}"
    if re_part == 0.0:
        return f"{im_part:.{digits}g}i"
    sign = "+" if im_part >= 0 else "-"
    return f"{re_part:.{digits}g} {sign} {abs(im_part):.{digits}g}i"


# ---------------------------------------------------------------------
# Core real square-root operations
# ---------------------------------------------------------------------

def principal_square_root_real(x: float) -> float:
    """
    Principal real square root.

    Contract:
        input x must be real and x >= 0
        output y >= 0 and y*y == x, within floating-point accuracy
    """
    if x < 0:
        raise DomainError(f"sqrt({x}) is not real. Use complex_square_root instead.")
    return math.sqrt(x)


def square_roots_of_equation_real(a: float) -> tuple[float, ...]:
    """
    Solve x^2 = a over real numbers.

    Difference from √a:
        √a returns only the principal nonnegative root.
        x² = a returns both signs when a > 0.
    """
    if a < 0:
        return ()
    r = math.sqrt(a)
    if r == 0.0:
        return (0.0,)
    return (-r, r)


def principal_square_root_complex(z: complex) -> complex:
    """Principal complex square root using Python's cmath convention."""
    return cmath.sqrt(z)


# ---------------------------------------------------------------------
# nth-root operations
# ---------------------------------------------------------------------

def principal_nth_root_complex(z: complex, n: int) -> complex:
    """
    Principal complex nth root.

    Uses polar form:
        z = r * exp(iθ)
        principal root = r^(1/n) * exp(iθ/n)

    where θ is the principal argument from cmath.polar.
    """
    if n < 2:
        raise ValueError("root index n must be >= 2")

    if z == 0:
        return 0j

    radius, theta = cmath.polar(z)
    return radius ** (1.0 / n) * cmath.exp(1j * theta / n)


def all_nth_roots_complex(z: complex, n: int) -> tuple[complex, ...]:
    """
    All complex solutions to:

        x^n = z

    There are n complex roots, counting multiplicity.
    """
    if n < 2:
        raise ValueError("root index n must be >= 2")

    if z == 0:
        return tuple(0j for _ in range(n))

    radius, theta = cmath.polar(z)
    root_radius = radius ** (1.0 / n)

    return tuple(
        root_radius * cmath.exp(1j * (theta + TAU * k) / n)
        for k in range(n)
    )


def principal_nth_root_real(x: float, n: int) -> float:
    """
    Principal real nth root when it exists naturally over reals.

    Rules:
        - n even and x < 0: no real principal root
        - n even and x >= 0: nonnegative root
        - n odd: one real root, sign follows x
    """
    if n < 2:
        raise ValueError("root index n must be >= 2")

    if x < 0 and n % 2 == 0:
        raise DomainError(f"even root index {n} of negative real {x} is not real.")

    if x < 0:
        return -((-x) ** (1.0 / n))

    return x ** (1.0 / n)


def real_roots_of_power_equation(a: float, n: int) -> tuple[float, ...]:
    """
    Solve x^n = a over real numbers.

    n odd:
        one real root for every real a

    n even:
        if a > 0: two real roots ±root
        if a = 0: one real root 0
        if a < 0: no real roots
    """
    if n < 2:
        raise ValueError("root index n must be >= 2")

    if n % 2 == 1:
        return (principal_nth_root_real(a, n),)

    if a < 0:
        return ()

    r = principal_nth_root_real(a, n)
    if r == 0.0:
        return (0.0,)
    return (-r, r)


# ---------------------------------------------------------------------
# Integer radical simplification
# ---------------------------------------------------------------------

def simplify_integer_square_root(n: int) -> SimplifiedSquareRoot:
    """
    Simplify an integer square root.

    Examples:
        √72  = 6√2
        √50  = 5√2
        √36  = 6
        √-72 = 6i√2

    Algorithm:
        factor out the largest square divisor.
    """
    if not isinstance(n, int):
        raise TypeError("n must be an int")

    if n == 0:
        return SimplifiedSquareRoot(original=n, coefficient=0, radicand=1)

    imaginary = n < 0
    remaining = abs(n)
    coefficient = 1

    # Factor square divisors p^2 greedily.
    p = 2
    while p * p <= remaining:
        square = p * p
        while remaining % square == 0:
            coefficient *= p
            remaining //= square
        p += 1 if p == 2 else 2  # after 2, check odd candidates

    return SimplifiedSquareRoot(
        original=n,
        coefficient=coefficient,
        radicand=remaining,
        imaginary_unit=imaginary,
    )


# ---------------------------------------------------------------------
# Radical punctuation / notation parser
# ---------------------------------------------------------------------

_SQRT_PATTERNS = [
    # √[3](8), √[3]{8}, √[3]8
    re.compile(r"^\s*√\s*\[\s*(?P<index>\d+)\s*\]\s*(?:\((?P<paren>.*)\)|\{(?P<brace>.*)\}|(?P<plain>.+))\s*$"),
    # √(25), √{25}, √25
    re.compile(r"^\s*√\s*(?:\((?P<paren>.*)\)|\{(?P<brace>.*)\}|(?P<plain>.+))\s*$"),
    # sqrt(25)
    re.compile(r"^\s*sqrt\s*\((?P<paren>.*)\)\s*$", re.IGNORECASE),
    # root(3, 8)
    re.compile(r"^\s*root\s*\(\s*(?P<index>\d+)\s*,\s*(?P<paren>.*)\)\s*$", re.IGNORECASE),
]


def parse_radical_notation(text: str) -> RadicalNotation:
    """
    Parse common radical input forms:

        √25
        √(25)
        √{25}
        sqrt(25)
        √[3](8)
        root(3, 8)

    This parser extracts notation. It does not do symbolic algebra.
    """
    if not text or not text.strip():
        raise ParseRadicalError("empty radical expression")

    for pattern in _SQRT_PATTERNS:
        match = pattern.match(text)
        if not match:
            continue

        groups = match.groupdict()
        index_raw = groups.get("index")
        index = int(index_raw) if index_raw else 2
        radicand = groups.get("paren") or groups.get("brace") or groups.get("plain")

        if radicand is None or radicand.strip() == "":
            raise ParseRadicalError("missing radicand")

        return RadicalNotation(index=index, radicand=radicand.strip())

    raise ParseRadicalError(
        "Could not parse radical notation. Try forms like √25, √(25), sqrt(25), √[3](8), root(3, 8)."
    )


def radical_punctuation_parts(notation: RadicalNotation) -> RadicalParts:
    return RadicalParts(
        radical_sign="√",
        index=notation.index,
        radicand=notation.radicand,
        vinculum_meaning="The bar/scope over the radicand says exactly what expression is inside the root.",
        principal_root_rule="A bare square root √x means the principal nonnegative root when x is a nonnegative real.",
    )


# ---------------------------------------------------------------------
# Safe numeric expression evaluator for radicands
# ---------------------------------------------------------------------

_ALLOWED_BINARY_OPERATORS: dict[type[ast.operator], Callable[[Any, Any], Any]] = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: operator.truediv,
    ast.Pow: operator.pow,
    ast.Mod: operator.mod,
}

_ALLOWED_UNARY_OPERATORS: dict[type[ast.unaryop], Callable[[Any], Any]] = {
    ast.UAdd: operator.pos,
    ast.USub: operator.neg,
}

_ALLOWED_NAMES: dict[str, float] = {
    "pi": math.pi,
    "tau": TAU,
    "e": math.e,
}


def safe_eval_number(expr: str) -> float:
    """
    Evaluate simple numeric arithmetic safely.

    Allowed:
        numbers, +, -, *, /, %, **, parentheses, pi, tau, e

    Disallowed:
        function calls, imports, attributes, variables other than pi/tau/e
    """
    try:
        parsed = ast.parse(expr, mode="eval")
    except SyntaxError as exc:
        raise ParseRadicalError(f"Invalid numeric expression: {expr!r}") from exc

    value = _eval_ast_node(parsed.body)

    if isinstance(value, bool):
        raise ParseRadicalError("Boolean values are not valid radicands here.")

    if not isinstance(value, (int, float)):
        raise ParseRadicalError("Expression did not evaluate to a real number.")

    return float(value)


def _eval_ast_node(node: ast.AST) -> int | float:
    if isinstance(node, ast.Constant):
        if isinstance(node.value, (int, float)) and not isinstance(node.value, bool):
            return node.value
        raise ParseRadicalError(f"Unsupported constant: {node.value!r}")

    if isinstance(node, ast.Name):
        if node.id in _ALLOWED_NAMES:
            return _ALLOWED_NAMES[node.id]
        raise ParseRadicalError(f"Name {node.id!r} is not allowed. Allowed names: pi, tau, e.")

    if isinstance(node, ast.UnaryOp):
        op_type = type(node.op)
        if op_type not in _ALLOWED_UNARY_OPERATORS:
            raise ParseRadicalError("Unsupported unary operator.")
        return _ALLOWED_UNARY_OPERATORS[op_type](_eval_ast_node(node.operand))

    if isinstance(node, ast.BinOp):
        op_type = type(node.op)
        if op_type not in _ALLOWED_BINARY_OPERATORS:
            raise ParseRadicalError("Unsupported binary operator.")

        left = _eval_ast_node(node.left)
        right = _eval_ast_node(node.right)

        # Prevent absurd exponent blowups in a small utility program.
        if isinstance(node.op, ast.Pow) and abs(right) > 1000:
            raise ParseRadicalError("Exponent too large.")

        return _ALLOWED_BINARY_OPERATORS[op_type](left, right)

    raise ParseRadicalError(f"Unsupported expression element: {type(node).__name__}")


# ---------------------------------------------------------------------
# Complete analysis pipeline
# ---------------------------------------------------------------------

def analyze_radical_expression(text: str) -> RootAnalysis:
    """
    Parse radical notation, evaluate numeric radicand, and compute roots.

    Example:
        analyze_radical_expression("√25")
        analyze_radical_expression("√[3](-8)")
        analyze_radical_expression("root(4, 16)")
    """
    notation = parse_radical_notation(text)
    radicand_value = safe_eval_number(notation.radicand)
    n = notation.index

    principal = principal_nth_root_complex(complex(radicand_value), n)
    complex_roots = all_nth_roots_complex(complex(radicand_value), n)
    real_roots = real_roots_of_power_equation(radicand_value, n)

    if n == 2:
        note = (
            "√x is the principal square root. The equation y² = x may have two real roots, ±√x, "
            "when x > 0."
        )
    else:
        note = (
            f"This is an index-{n} radical. The equation y^{n} = x has {n} complex roots, "
            "but the radical symbol usually selects a principal branch/value."
        )

    return RootAnalysis(
        notation=notation,
        radicand_value=radicand_value,
        principal_value=principal,
        all_complex_roots=complex_roots,
        all_real_roots=real_roots,
        note=note,
    )


# ---------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------

def print_analysis(analysis: RootAnalysis) -> None:
    parts = radical_punctuation_parts(analysis.notation)

    print("\nRADICAL / SQUARE-ROOT ANALYSIS")
    print("=" * 46)
    print(f"input notation:         {analysis.notation.render_unicode()}")
    print(f"ASCII notation:         {analysis.notation.render_ascii()}")
    print(f"LaTeX notation:         {analysis.notation.render_latex()}")
    print()
    print("PUNCTUATION PARTS")
    print("-" * 46)
    print(f"radical sign:           {parts.radical_sign}")
    print(f"index:                  {parts.index}")
    print(f"radicand:               {parts.radicand}")
    print(f"vinculum/scope rule:    {parts.vinculum_meaning}")
    print(f"principal-root rule:    {parts.principal_root_rule}")
    print()
    print("VALUES")
    print("-" * 46)
    print(f"numeric radicand:       {analysis.radicand_value}")
    print(f"principal value:        {format_complex(analysis.principal_value)}")
    print("real equation roots:    ", end="")
    if analysis.all_real_roots:
        print(", ".join(f"{x:.12g}" for x in analysis.all_real_roots))
    else:
        print("none")
    print("complex equation roots: ", end="")
    print(", ".join(format_complex(z) for z in analysis.all_complex_roots))
    print()
    print("NOTE")
    print("-" * 46)
    print(analysis.note)


# ---------------------------------------------------------------------
# Demonstration and tests
# ---------------------------------------------------------------------

def demo() -> None:
    print("\nDEMO: SQUARE ROOT / RADICAL PUNCTUATION")
    print("=" * 56)

    examples = [
        "√25",
        "sqrt(2)",
        "√(9 + 16)",
        "√[-should fail]",  # intentionally invalid in parser pattern, skipped below
        "√[3](-8)",
        "root(4, 16)",
        "√(-9)",
    ]

    for text in examples:
        if "should fail" in text:
            continue
        analysis = analyze_radical_expression(text)
        print_analysis(analysis)

    print("\nINTEGER SIMPLIFICATION")
    print("=" * 56)
    for n in [0, 1, 2, 8, 12, 18, 36, 50, 72, -72, 200]:
        simplified = simplify_integer_square_root(n)
        print(f"√{n:<4} = {simplified.render_unicode():<8}    ASCII: {simplified.render_ascii()}")

    print("\nDIRECT STANDARD CALLS")
    print("=" * 56)
    print(f"principal_square_root_real(81) = {principal_square_root_real(81)}")
    print(f"square_roots_of_equation_real(81) = {square_roots_of_equation_real(81)}")
    print(f"principal_square_root_complex(-81) = {format_complex(principal_square_root_complex(-81))}")
    print(f"all_nth_roots_complex(1, 4) = {[format_complex(z) for z in all_nth_roots_complex(1, 4)]}")


def assert_close_complex(a: complex, b: complex, eps: float = 1e-10) -> None:
    if abs(a - b) > eps:
        raise AssertionError(f"expected {b}, got {a}")


def selftest() -> None:
    # Principal square root rules
    assert principal_square_root_real(0.0) == 0.0
    assert principal_square_root_real(9.0) == 3.0

    try:
        principal_square_root_real(-1.0)
    except DomainError:
        pass
    else:
        raise AssertionError("negative real square root should raise DomainError")

    # Equation roots
    assert square_roots_of_equation_real(0.0) == (0.0,)
    assert square_roots_of_equation_real(9.0) == (-3.0, 3.0)
    assert square_roots_of_equation_real(-9.0) == ()

    # Complex square roots
    assert_close_complex(principal_square_root_complex(-9), 3j)

    # nth roots
    assert principal_nth_root_real(27.0, 3) == 3.0
    assert round(principal_nth_root_real(-27.0, 3), 10) == -3.0
    assert real_roots_of_power_equation(16.0, 4) == (-2.0, 2.0)
    assert real_roots_of_power_equation(-16.0, 4) == ()

    roots = all_nth_roots_complex(1, 4)
    for root in roots:
        assert_close_complex(root ** 4, 1.0 + 0j, eps=1e-9)

    # Integer simplification
    s72 = simplify_integer_square_root(72)
    assert s72.coefficient == 6
    assert s72.radicand == 2
    assert not s72.imaginary_unit
    assert s72.render_unicode() == "6√2"

    sm72 = simplify_integer_square_root(-72)
    assert sm72.coefficient == 6
    assert sm72.radicand == 2
    assert sm72.imaginary_unit
    assert sm72.render_unicode() == "6i√2"

    s36 = simplify_integer_square_root(36)
    assert s36.coefficient == 6
    assert s36.radicand == 1
    assert s36.render_unicode() == "6"

    # Parser
    assert parse_radical_notation("√25") == RadicalNotation("25")
    assert parse_radical_notation("sqrt(25)") == RadicalNotation("25")
    assert parse_radical_notation("√[3](8)") == RadicalNotation("8", index=3)
    assert parse_radical_notation("root(4, 16)") == RadicalNotation("16", index=4)

    # Safe evaluator
    assert safe_eval_number("9 + 16") == 25.0
    assert safe_eval_number("2**3") == 8.0
    assert round(safe_eval_number("pi"), 12) == round(math.pi, 12)

    try:
        safe_eval_number("__import__('os').system('echo bad')")
    except ParseRadicalError:
        pass
    else:
        raise AssertionError("unsafe expression should not be allowed")

    # Full analysis
    a = analyze_radical_expression("√25")
    assert a.radicand_value == 25.0
    assert_close_complex(a.principal_value, 5.0 + 0j)
    assert a.all_real_roots == (-5.0, 5.0)

    b = analyze_radical_expression("√[3](-8)")
    assert b.radicand_value == -8.0
    assert b.all_real_roots == (-2.0,)

    print("All self-tests passed.")


# ---------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Square-root / radical-notation complete programming lab."
    )

    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("demo", help="Run the built-in demonstration.")
    sub.add_parser("selftest", help="Run built-in self-tests.")

    parse_cmd = sub.add_parser("parse", help="Parse and analyze radical notation.")
    parse_cmd.add_argument("expression", help="Example: '√25', 'sqrt(25)', '√[3](8)', 'root(4, 16)'")

    simplify_cmd = sub.add_parser("simplify", help="Simplify an integer square root.")
    simplify_cmd.add_argument("integer", type=int, help="Integer under the square root, e.g. 72 or -72.")

    roots_cmd = sub.add_parser("roots", help="Solve x^n = value.")
    roots_cmd.add_argument("value", type=float, help="Right-hand value a in x^n = a.")
    roots_cmd.add_argument("index", type=int, help="Power/root index n.")

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    try:
        if args.command == "demo":
            demo()
            return 0

        if args.command == "selftest":
            selftest()
            return 0

        if args.command == "parse":
            analysis = analyze_radical_expression(args.expression)
            print_analysis(analysis)
            return 0

        if args.command == "simplify":
            simplified = simplify_integer_square_root(args.integer)
            print(f"√{args.integer} = {simplified.render_unicode()}")
            print(f"ASCII: sqrt({args.integer}) = {simplified.render_ascii()}")
            return 0

        if args.command == "roots":
            value = args.value
            n = args.index
            print(f"Solving x^{n} = {value}")
            real_roots = real_roots_of_power_equation(value, n)
            complex_roots = all_nth_roots_complex(complex(value), n)

            print("Real roots:")
            if real_roots:
                for root in real_roots:
                    print(f"  {root:.12g}")
            else:
                print("  none")

            print("Complex roots:")
            for root in complex_roots:
                print(f"  {format_complex(root)}")
            return 0

        parser.error(f"Unknown command {args.command!r}")
        return 2

    except RadicalError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
