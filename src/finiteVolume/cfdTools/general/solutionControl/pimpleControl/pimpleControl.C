/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2018 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "pimpleControl.H"
#include "Switch.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
    defineTypeNameAndDebug(pimpleControl, 0);
}


// * * * * * * * * * * * * Protected Member Functions  * * * * * * * * * * * //

bool Foam::pimpleControl::read()
{
    bool ok = solutionControl::read();

    if (ok)
    {
        const dictionary& pimpleDict = dict();

        solveFlow_ = pimpleDict.lookupOrDefault<Switch>("solveFlow", true);
        nCorrPIMPLE_ = pimpleDict.lookupOrDefault<label>("nOuterCorrectors", 1);
        nCorrPISO_ = pimpleDict.lookupOrDefault<label>("nCorrectors", 1);
        SIMPLErho_ = pimpleDict.lookupOrDefault<Switch>("SIMPLErho", false);
        turbOnFinalIterOnly_ =
            pimpleDict.lookupOrDefault<Switch>("turbOnFinalIterOnly", true);
    }

    return ok;
}


bool Foam::pimpleControl::criteriaSatisfied()
{
    // no checks on first iteration - nothing has been calculated yet
    if ((corr_ == 1) || residualControl_.empty() || finalIter())
    {
        return false;
    }


    bool storeIni = this->storeInitialResiduals();

    bool achieved = true;
    bool checked = false;    // safety that some checks were indeed performed

    const dictionary& solverDict = mesh_.solverPerformanceDict();
    forAllConstIter(dictionary, solverDict, iter)
    {
        const word& variableName = iter().keyword();
        const label fieldi = applyToField(variableName);
        if (fieldi != -1)
        {
            scalar residual = 0;
            const scalar firstResidual =
                maxResidual(variableName, iter().stream(), residual);

            checked = true;

            if (storeIni)
            {
                residualControl_[fieldi].initialResidual = firstResidual;
            }

            const bool absCheck = residual < residualControl_[fieldi].absTol;
            bool relCheck = false;

            scalar relative = 0.0;
            if (!storeIni)
            {
                const scalar iniRes =
                    residualControl_[fieldi].initialResidual
                  + rootVSmall;

                relative = residual/iniRes;
                relCheck = relative < residualControl_[fieldi].relTol;
            }

            achieved = achieved && (absCheck || relCheck);

            if (debug)
            {
                Info<< algorithmName_ << " loop:" << endl;

                Info<< "    " << variableName
                    << " PIMPLE iter " << corr_
                    << ": ini res = "
                    << residualControl_[fieldi].initialResidual
                    << ", abs tol = " << residual
                    << " (" << residualControl_[fieldi].absTol << ")"
                    << ", rel tol = " << relative
                    << " (" << residualControl_[fieldi].relTol << ")"
                    << endl;
            }
        }
    }

    return checked && achieved;
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::pimpleControl::pimpleControl(fvMesh& mesh, const word& dictName)
:
    solutionControl(mesh, dictName),
    solveFlow_(true),
    nCorrPIMPLE_(0),
    nCorrPISO_(0),
    corrPISO_(0),
    SIMPLErho_(false),
    turbOnFinalIterOnly_(true),
    converged_(false)
{
    read();

    if (nCorrPIMPLE_ > 1)
    {
        Info<< nl;
        if (residualControl_.empty())
        {
            Info<< algorithmName_ << ": no residual control data found. "
                << "Calculations will employ " << nCorrPIMPLE_
                << " corrector loops" << nl << endl;
        }
        else
        {
            Info<< algorithmName_ << ": max iterations = " << nCorrPIMPLE_
                << endl;
            forAll(residualControl_, i)
            {
                Info<< "    field " << residualControl_[i].name << token::TAB
                    << ": relTol " << residualControl_[i].relTol
                    << ", tolerance " << residualControl_[i].absTol
                    << nl;
            }
            Info<< endl;
        }
    }
    else
    {
        Info<< nl << algorithmName_ << ": Operating solver in PISO mode" << nl
            << endl;
    }
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::pimpleControl::~pimpleControl()
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

bool Foam::pimpleControl::loop()
{
    read();

    corr_++;

    if (debug)
    {
        Info<< algorithmName_ << " loop: corr = " << corr_ << endl;
    }

    if (corr_ == nCorrPIMPLE_ + 1)
    {
        if ((!residualControl_.empty()) && (nCorrPIMPLE_ != 1))
        {
            Info<< algorithmName_ << ": not converged within "
                << nCorrPIMPLE_ << " iterations" << endl;
        }

        corr_ = 0;
        mesh_.data::remove("finalIteration");
        return false;
    }

    bool completed = false;
    if (converged_ || criteriaSatisfied())
    {
        if (converged_)
        {
            Info<< algorithmName_ << ": converged in " << corr_ - 1
                << " iterations" << endl;

            mesh_.data::remove("finalIteration");
            corr_ = 0;
            converged_ = false;

            completed = true;
        }
        else
        {
            Info<< algorithmName_ << ": iteration " << corr_ << endl;
            storePrevIterFields();

            mesh_.data::add("finalIteration", true);
            converged_ = true;
        }
    }
    else
    {
        if (finalIter())
        {
            mesh_.data::add("finalIteration", true);
        }

        if (corr_ <= nCorrPIMPLE_)
        {
            Info<< algorithmName_ << ": iteration " << corr_ << endl;
            storePrevIterFields();
            completed = false;
        }
    }

    return !completed;
}


// ************************************************************************* //
