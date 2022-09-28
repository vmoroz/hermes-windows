/*
 * This script sets the version number for the rest of the build.
 * After this script has run, other tasks can use the variable
 * to retrieve the build number for their logic...
 * 
 * See: https://docs.microsoft.com/en-us/azure/devops/pipelines/build/variables?view=azure-devops&tabs=yaml
 * for environment variables used in this script to compute the version number.
 */ 
const env = process.env;

function main() {

    if (env["Build_Reason"] === "PullRequest") {
        fatalError("Build script is intended for CI pipeline and should not be used for pull requests.");
    }

    const {semanticVersion, fileVersion} = computeVersion();
    console.log(`Semantic Version: ${semanticVersion}`);
    console.log(`Windows File Version: ${fileVersion}`);

    // Update the build number so the pipelines so we can easily correlate builds and releases.
    console.log(`##vso[build.updatebuildnumber]${fileVersion} -- ${semanticVersion}`);

    // Set the variables (as output) so that other jobs can use them.
    console.log(`##vso[task.setvariable variable=semanticVersion;isOutput=true]${semanticVersion}`);
    console.log(`##vso[task.setvariable variable=fileVersion;isOutput=true]${fileVersion}`);
}

function computeVersion() {
    // Compute base version;
    const sourceBranch = env["Build_SourceBranch"];
    switch (sourceBranch) {
        case "refs/heads/testing/vmorozov/Version": // $TODO(vmoroz): Remove this for production :)
        case "refs/heads/main":
            return computeMainVersion();

        // $TODO: VMoroz: Add logic for expected versions in release branches.

        default:
            fatalError(`Build script does not support source branch '${sourceBranch}'.`)
    }
    
}

function computeMainVersion() {
    const buildNumber = env["Build_BuildNumber"];
    const buildNumberParts = buildNumber.split(".");
    if (buildNumberParts.length !== 4
        || buildNumberParts[0] !== '0'
        || buildNumberParts[1] !== '0'
        || buildNumberParts[2].length !== 4
        || buildNumberParts[3].length < 4
        || buildNumberParts[3].length > 5) {
        fatalError(`Unexpected build number format encountered: ${buildNumber}`)
    }

    const dayAndSequenceNumber = buildNumberParts[3] === 4 ? '0' + buildNumberParts[3] : buildNumberParts[3];
    const shortGitHash = env["Build_SourceVersion"].substring(0, 8);

    return {
        semanticVersion: `0.0.0-${buildNumberParts[2]}${dayAndSequenceNumber}-${shortGitHash}`,
        fileVersion: buildNumber
    }
}

function fatalError(message) {
    console.log(`##[error]${message}`);
    process.exit(1);
}

main();