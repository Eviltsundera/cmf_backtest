# Entrance Exam Task

## Goal

Develop a backtesting engine that can replay historical limit-order-book data
and evaluate a market-making strategy.

## Programming Language

C++ or Python.

## Minimum Requirements

- Limit-order-book simulation.
- Limit order placement and cancellation.
- Order execution simulation.
- Partial fills are optional.

Baseline metrics:

- PnL.
- Inventory.
- Turnover.

## Strategy Implementation

- Implement Avellaneda-Stoikov (2008).
- Improve it with microprice plus Avellaneda-Stoikov (2018) extensions.
- Run simulation experiments.

## Execution Assumption

An order is executed when the market price crosses the order level.

## Expected Results

Backtester deliverables:

- Integrated backtesting engine.
- Sample dataset and configs.
- Performance report.
- Technical documentation.

Strategy deliverables:

- Source code.
- Model description.
- Performance results.
- Improvement roadmap.

## Simulation Data

Historical market data is supplied separately as `MD.zip`.

## Submission Form

Submit the final package through the provided application form.
