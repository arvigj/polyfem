#pragma once

#include <polyfem/solver/forms/Form.hpp>
#include "VariableToSimulation.hpp"

namespace polyfem::solver
{
	class AdjointForm : public Form
	{
	public:
		AdjointForm(const std::vector<std::shared_ptr<VariableToSimulation>> &variable_to_simulations) : variable_to_simulations_(variable_to_simulations) {}
		virtual ~AdjointForm() {}

		double value(const Eigen::VectorXd &x) const override;

		void enable_energy_print(const std::string &print_energy_keyword) { print_energy_keyword_ = print_energy_keyword; print_energy_ = 1; }

		virtual void solution_changed(const Eigen::VectorXd &new_x) override;

		const auto &get_variable_to_simulations() const { return variable_to_simulations_; }

		virtual Eigen::MatrixXd compute_adjoint_rhs(const Eigen::VectorXd &x, const State &state) const final
		{
			return compute_reduced_adjoint_rhs_unweighted(x, state) * weight_;
		}

		virtual void compute_partial_gradient(const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const final
		{
			compute_partial_gradient_unweighted(x, gradv);
			gradv *= weight_;
		}

		virtual Eigen::MatrixXd compute_adjoint_rhs_unweighted(const Eigen::VectorXd &x, const State &state) const;
		virtual Eigen::MatrixXd compute_reduced_adjoint_rhs_unweighted(const Eigen::VectorXd &x, const State &state) const;
		virtual void compute_partial_gradient_unweighted(const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const;

		virtual void second_derivative_unweighted(const Eigen::VectorXd &x, StiffnessMatrix &hessian) const final override;

	protected:
		virtual void first_derivative_unweighted(const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const override;

		std::vector<std::shared_ptr<VariableToSimulation>> variable_to_simulations_;

		mutable int print_energy_ = 0; // 0: don't print, 1: print, 2: already printed on current solution
		std::string print_energy_keyword_;
	};

	class StaticForm : public AdjointForm
	{
	public:
		using AdjointForm::AdjointForm;
		virtual ~StaticForm() = default;

		Eigen::MatrixXd compute_adjoint_rhs_unweighted(const Eigen::VectorXd &x, const State &state) const final override;
		void compute_partial_gradient_unweighted(const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const final override;
		double value_unweighted(const Eigen::VectorXd &x) const final override;

		virtual Eigen::VectorXd compute_adjoint_rhs_unweighted_step(const int time_step, const Eigen::VectorXd &x, const State &state) const = 0;
		virtual void compute_partial_gradient_unweighted_step(const int time_step, const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const = 0;
		virtual double value_unweighted_step(const int time_step, const Eigen::VectorXd &x) const = 0;
		virtual void solution_changed_step(const int time_step, const Eigen::VectorXd &new_x) {}
	};

	class MaxStressForm : public StaticForm
	{
	public:
		MaxStressForm(const std::vector<std::shared_ptr<VariableToSimulation>> &variable_to_simulations, const State &state, const json &args) : StaticForm(variable_to_simulations), state_(state)
		{
			auto tmp_ids = args["volume_selection"].get<std::vector<int>>();
			interested_ids_ = std::set(tmp_ids.begin(), tmp_ids.end());
		}

		Eigen::VectorXd compute_adjoint_rhs_unweighted_step(const int time_step, const Eigen::VectorXd &x, const State &state) const override;
		double value_unweighted_step(const int time_step, const Eigen::VectorXd &x) const override;
		void compute_partial_gradient_unweighted_step(const int time_step, const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const override;
	
	private:
		std::set<int> interested_ids_;
		const State &state_;
	};

	class HomogenizedDispGradForm : public AdjointForm
	{
	public:
		HomogenizedDispGradForm(const std::vector<std::shared_ptr<VariableToSimulation>> &variable_to_simulations, const State &state, const json &args) : AdjointForm(variable_to_simulations), state_(state)
		{
			dimensions_ = args["dimensions"].get<std::vector<int>>();
		}

		double value_unweighted(const Eigen::VectorXd &x) const override;
		Eigen::MatrixXd compute_reduced_adjoint_rhs_unweighted(const Eigen::VectorXd &x, const State &state) const override;

	protected:
		std::vector<int> dimensions_;
		const State &state_;
	};

	class WeightedSolution : public AdjointForm
	{
	public:
		WeightedSolution(const std::vector<std::shared_ptr<VariableToSimulation>> &variable_to_simulations, const State &state, const json &args);

		double value_unweighted(const Eigen::VectorXd &x) const override;
		Eigen::MatrixXd compute_adjoint_rhs_unweighted(const Eigen::VectorXd &x, const State &state) const override;
		void compute_partial_gradient_unweighted(const Eigen::VectorXd &x, Eigen::VectorXd &gradv) const override;
	private:
		Eigen::VectorXd coeffs;
		const State &state_;
	};
} // namespace polyfem::solver