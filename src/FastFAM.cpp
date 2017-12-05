/*
   GCTA: a tool for Genome-wide Complex Trait Analysis

   FastFAM regression

   Depends on the class of genotype

   Developed by Zhili Zheng<zhilizheng@outlook.com>

   This file is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   A copy of the GNU General Public License is attached along with this program.
   If not, see <http://www.gnu.org/licenses/>.
*/
#include "FastFAM.h"
#include "main/StatFunc.h"
#include <cmath>
#include <algorithm>
#include <Eigen/SparseCholesky>
#include <sstream>
#include <iterator>
#include "utils.hpp"
#include "Logger.h"
#include "ThreadPool.h"


map<string, string> FastFAM::options;
map<string, double> FastFAM::options_d;
vector<string> FastFAM::processFunctions;

FastFAM::FastFAM(Geno *geno){
    Eigen::setNbThreads(THREADS.getThreadCount() + 1);
    this->geno = geno;
    num_indi = geno->pheno->count_keep();
    num_marker = geno->marker->count_extract();

    beta = new double[num_marker];
    se = new double[num_marker];
    p = new double[num_marker];

    double VG;
    double VR;
    bool flag_est_GE = true;
    if(options.find("G") != options.end()){
        VG = std::stod(options["G"]);
        VR = std::stod(options["E"]);
        flag_est_GE = false;
    }

    vector<string> ids;
    geno->pheno->get_pheno(ids, phenos);
    LOGGER.i(0, "After removing NAs in phenotype, there are " + to_string(ids.size()) + " subjects remained");
    if(ids.size() != num_indi){
        LOGGER.e(0, "Phenotype is not equal, this shall be a flag bug");
    }

    // read covar
    vector<uint32_t> remain_index, remain_index_covar;
    vector<vector<double>> v_covar;
    bool has_qcovar = false;
    if(options.find("concovar") != options.end()){
        has_qcovar = true;
        LOGGER.i(0, "Reading covariance...");
        vector<string> v_covar_id = Pheno::read_sublist(options["concovar"], &v_covar); 
        vector_commonIndex(ids, v_covar_id, remain_index, remain_index_covar);
        LOGGER.i(0, "After merging, " + to_string(remain_index.size()) + " subjects remained");
    }else{
        remain_index.resize(ids.size());
        std::iota(remain_index.begin(), remain_index.end(), 0);
    }

    vector<string> remain_ids(remain_index.size());
    std::transform(remain_index.begin(), remain_index.end(), remain_ids.begin(), [&ids](size_t pos){return ids[pos];});

    // read fam
    string ffam_file = options["grmsparse_file"];
    vector<uint32_t> remain_index_fam;
    SpMat fam;
    readFAM(ffam_file, fam, remain_ids, remain_index_fam);

    int n_remain_index_fam = remain_index_fam.size();

    //reorder phenotype, covar
    vector<double> remain_phenos(n_remain_index_fam);
    for(int i = 0; i != n_remain_index_fam; i++){
        remain_phenos[i] = phenos[remain_index[remain_index_fam[i]]];
    }

    vector<double> remain_covar;
    if(has_qcovar){
        vector<uint32_t> remain_index_covar_fam(n_remain_index_fam, 0);
        int n_v_covar = v_covar.size();
        std::transform(remain_index_fam.begin(), remain_index_fam.end(), remain_index_covar_fam.begin(), 
            [&remain_index, &remain_index_covar](size_t pos){
                ptrdiff_t vector_pos = std::find(remain_index.begin(), remain_index.end(), pos) - remain_index.begin();
                return remain_index_covar[vector_pos];
            });

        remain_covar.resize(n_remain_index_fam * (n_v_covar + 1));

        for(int j = 0; j != n_v_covar; j++){
            int base_index = (j + 1) * n_remain_index_fam;
            for(int i = 0; i != n_remain_index_fam; i++){
                remain_covar[base_index + i] = v_covar[j][remain_index_covar_fam[i]];
            }
        }

        for(int i = 0; i != n_remain_index_fam; i++){
            remain_covar[i] = 1.0;
        }
    }

    // standerdize the phenotype, and condition the covar
    phenoVec = Map<VectorXd> (remain_phenos.data(), remain_phenos.size());
    // condition the covar
    if(has_qcovar){
        MatrixXd concovar = Map<Matrix<double, Dynamic, Dynamic, ColMajor>>(remain_covar.data(), remain_phenos.size(), v_covar.size() + 1);
        conditionCovarReg(phenoVec, concovar);
    }

    // Center
    double phenoVec_mean = phenoVec.mean();
    phenoVec -= VectorXd::Ones(phenoVec.size()) * phenoVec_mean;

    double Vpheno = phenoVec.array().square().sum() / (phenoVec.size() - 1);
    //phenoVec /= pheno_sd;
    
    LOGGER.i(0, "DEBUG: conditioned Pheno (first 5)");

    for(int i = 0; i < 5; i++){
        LOGGER.i(0, to_string(phenoVec[i]));
    }

    vector<double> Aij;
    vector<double> Zij;
 
    if(flag_est_GE){
        LOGGER.i(0, "Estimate VG by HE regression");
        for(int k = 0; k < fam.outerSize(); ++k){
            for(SpMat::InnerIterator it(fam, k); it; ++it){
                if(it.row() < it.col()){
                    Aij.push_back(it.value());
                    Zij.push_back(phenoVec[it.row()] * phenoVec[it.col()]);
                }
            }
        }

        VG = HEreg(Zij, Aij);
        VR = Vpheno - VG;
        LOGGER.i(2, "Vg=" + to_string(VG) + ", Ve=" + to_string(VR));
        LOGGER.i(2, "hsq=" + to_string(VG/Vpheno));
    }

    inverseFAM(fam, VG, VR);
}

void FastFAM::conditionCovarReg(VectorXd &pheno, MatrixXd &covar){
    MatrixXd t_covar = covar.transpose();
    VectorXd beta = (t_covar * covar).ldlt().solve(t_covar * pheno);
    std::cout << "DEBUG: betas" << std::endl;
    std::cout << beta << std::endl;
    //VectorXd beta = covar.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(pheno);
    pheno -= covar * beta;
    //double pheno_mean = pheno.mean();
    //pheno -= (VectorXd::Ones(pheno.size())) * pheno_mean;
}

double FastFAM::HEreg(vector<double> &Zij, vector<double> &Aij){
    Map<VectorXd> ZVec(Zij.data(), Zij.size());
    Map<VectorXd> AVec(Aij.data(), Aij.size());

    double Zmean = ZVec.mean();
    double Amean = AVec.mean();
    ZVec -= (VectorXd::Ones(ZVec.size())) * Zmean;
    AVec -= (VectorXd::Ones(AVec.size())) * Amean;

    double A2v = (AVec.transpose() * AVec)(0, 0);
    if(A2v < 1e-6){
        LOGGER.e(0, "can't solve the regression");
    }
    double AZ = (AVec.transpose() * ZVec)(0, 0);
    double hsq = (1.0 / A2v) * AZ;

    VectorXd RZVec = ZVec - AVec * hsq;

    double delta = RZVec.array().square().sum() / (RZVec.size() - 2);
    double se = sqrt(delta / A2v);

    double z = hsq / se;

    double p = StatFunc::pchisq(z * z, 1);

    LOGGER.i(2, "beta: " + to_string(hsq) + ", se: " + to_string(se) +  ", P: " + to_string(p));

    if(p > 0.05){
        LOGGER.e(0, "the number of relatives is not large enough to run fastFAM");
    }

    return hsq;
}
    

void FastFAM::readFAM(string filename, SpMat& fam, const vector<string> &ids, vector<uint32_t> &remain_index){
    uint32_t num_indi = ids.size();
    vector<string> sublist = Pheno::read_sublist(filename + ".grm.id");
    vector<uint32_t> fam_index;
    vector_commonIndex(sublist, ids, fam_index, remain_index);
    LOGGER.i(0, "After merging, " + to_string(sublist.size()) + " subjects remained");

    //Fix index order to outside, that fix the phenotype, covar order
    //We should avoid reorder the GRM sparese, this costs much time. 
    vector<size_t> index_list_order = sort_indexes(fam_index);
    vector<uint32_t> ordered_fam_index(fam_index.size(), 0);
    vector<uint32_t> ordered_remain_index(fam_index.size(), 0);
    std::transform(index_list_order.begin(), index_list_order.end(), ordered_fam_index.begin(), [&fam_index](size_t pos){
            return fam_index[pos];});
    std::transform(index_list_order.begin(), index_list_order.end(), ordered_remain_index.begin(), [&remain_index](size_t pos){
            return remain_index[pos];});
    remain_index = ordered_remain_index;

    std::ifstream pair_list((filename + ".grm.sp").c_str());
    if(!pair_list){
        LOGGER.e(0, "can't read [" + filename + ".grm.sp]");
    }

    string line;
    int line_number = 0;
    int last_length = 0;

    vector<uint32_t> id1;
    vector<uint32_t> id2;
    vector<double> grm;

    vector<uint32_t> num_elements(num_indi, 0);

    map<uint32_t, uint32_t> map_index;
    for(uint32_t index = 0; index != ordered_fam_index.size(); index++){
        map_index[ordered_fam_index[index]] = index;
    }

    uint32_t tmp_id1 = 0, tmp_id2 = 0;
    double tmp_grm = 0.0;

    while(std::getline(pair_list, line)){
        line_number++;
        std::istringstream line_buf(line);
        std::istream_iterator<string> begin(line_buf), end;
        vector<string> line_elements(begin, end);

        tmp_id1 = (std::stoi(line_elements[0]));
        tmp_id2 = (std::stoi(line_elements[1]));
        if(map_index.find(tmp_id1) != map_index.end() &&
                map_index.find(tmp_id2) != map_index.end()){
            tmp_id1 = map_index[tmp_id1];
            tmp_id2 = map_index[tmp_id2];

            tmp_grm = std::stod(line_elements[2]);
            id1.push_back(tmp_id1);
            id2.push_back(tmp_id2);
            num_elements[tmp_id2] += 1;
            grm.push_back(tmp_grm);
            if(tmp_id1 != tmp_id2){
                id1.push_back(tmp_id2);
                id2.push_back(tmp_id1);
                num_elements[tmp_id1] += 1;
                grm.push_back(tmp_grm);
            }
        }
    }
    pair_list.close();

    auto sorted_index = sort_indexes(id2, id1);

    fam.resize(ordered_fam_index.size(), ordered_fam_index.size());
    fam.reserve(num_elements);

    for(auto index : sorted_index){
        fam.insertBackUncompressed(id1[index], id2[index]) = grm[index];
    }
    fam.finalize();
    fam.makeCompressed();

}

void FastFAM::inverseFAM(SpMat& fam, double VG, double VR){
    LOGGER.i(0, "Inversing the FAM, this may take long time");
    LOGGER.i(0, "Inverse Threads " + to_string(Eigen::nbThreads()));
    LOGGER.ts("INVERSE_FAM");
    SpMat eye(fam.rows(), fam.cols());
    eye.setIdentity();

    // V
    fam *= VG;
    fam += eye * VR;

    Eigen::SimplicialLDLT<SpMat> solver;
    solver.compute(fam);

    if(solver.info() != Eigen::Success){
        LOGGER.e(0, "can't inverse the FAM");
    }

    V_inverse = solver.solve(eye);
    LOGGER.i(0, "FAM inversed in " + to_string(LOGGER.tp("INVERSE_FAM")) + " seconds");
}


void FastFAM::calculate_fam(uint8_t *buf, int num_marker){
    // Memory fam_size * 2 * 4 + (N * 8 * 2 ) * thread_num + M * 3 * 8  B
    int num_thread = THREADS.getThreadCount() + 1; 

    int num_marker_part = (num_marker + num_thread - 1) / num_thread;
    for(int index = 0; index != num_thread - 1; index++){
        THREADS.AddJob(std::bind(&FastFAM::reg_thread, this, buf, index * num_marker_part, (index + 1) * num_marker_part));
    }

    reg_thread(buf, (num_thread - 1) * num_marker_part, num_marker);

    THREADS.WaitAll();

    num_finished_marker += num_marker;
    if(num_finished_marker % 30000 == 0){
        LOGGER.i(2, to_string(num_finished_marker) + " markers finished"); 
    }
}

void FastFAM::reg_thread(uint8_t *buf, int from_marker, int to_marker){
    Eigen::setNbThreads(1);
    double *w_buf = new double[num_indi];
    Map< VectorXd > xMat(w_buf, num_indi);
    MatrixXd XMat_V;
    for(int cur_marker = from_marker; cur_marker < to_marker; cur_marker++){
        geno->makeMarkerX(buf, cur_marker, w_buf, true, false);
        // Xt * V-1
        MatrixXd xMat_V = xMat.transpose() * V_inverse;
        // 
        double xMat_V_x = 1.0 / (xMat_V * xMat)(0, 0);
        double xMat_V_p = (xMat_V * phenoVec)(0, 0);
        
        double temp_beta =  xMat_V_x * xMat_V_p;
        double temp_se = sqrt(xMat_V_x);
        double temp_z = temp_beta / temp_se;

        uint32_t cur_raw_marker = num_finished_marker + cur_marker;

        beta[cur_raw_marker] = temp_beta; //* geno->RDev[cur_raw_marker]; 
        se[cur_raw_marker] = temp_se;
        {
            std::lock_guard<std::mutex> lock(chisq_lock);
            p[cur_raw_marker] = StatFunc::pchisq(temp_z * temp_z, 1); 
        } 
    }
    delete[] w_buf;
}


void FastFAM::output(string filename){
    //TODO get the real effect
    std::ofstream out(filename.c_str());
    vector<string> header{"CHR", "SNP", "POS", "A1", "A2", "AF1", "beta", "se", "p"};
    std::copy(header.begin(), header.end(), std::ostream_iterator<string>(out, "\t"));
    out << std::endl;
    for(int index = 0; index != num_marker; index++){
        out << geno->marker->get_marker(geno->marker->getExtractIndex(index)) << "\t" <<
            geno->AFA1[index] << "\t" << beta[index] << "\t" << se[index] << "\t" << p[index] << std::endl;
    }
    out.close();
    LOGGER.i(0, "Success:", "saved result to [" + filename +"]");
}

int FastFAM::registerOption(map<string, vector<string>>& options_in){
    int returnValue = 0;
    options["out"] = options_in["out"][0] + ".fastFAM.assoc";

    string curFlag = "--fastFAM";
    if(options_in.find(curFlag) != options_in.end()){
        processFunctions.push_back("fast_fam");
        returnValue++;
        options_in.erase(curFlag);
    }

    curFlag = "--grm-sparse";
    if(options_in.find(curFlag) != options_in.end()){
        if(options_in[curFlag].size() == 1){
            options["grmsparse_file"] = options_in[curFlag][0];
        }else{
            LOGGER.e(0, curFlag + "can't deal with 0 or > 1 files");
        }
        options_in.erase(curFlag);
    }

    curFlag = "--ge";
    if(options_in.find(curFlag) != options_in.end()){
        if(options_in[curFlag].size() == 2){
            options["G"] = options_in[curFlag][0];
            options["E"] = options_in[curFlag][1];
        }else{
            LOGGER.e(0, curFlag + " can't handle other than 2 numbers");
        }
        options_in.erase(curFlag);
    }

    curFlag = "--qcovar";
    if(options_in.find(curFlag) != options_in.end()){
        if(options_in[curFlag].size() == 1){
            options["concovar"] = options_in[curFlag][0];
        }else{
            LOGGER.e(0, curFlag + "can't deal with covar other than 1");
        }
    }

    return returnValue;
}

void FastFAM::processMain(){
    vector<function<void (uint8_t *, int)>> callBacks;
    for(auto &process_function : processFunctions){
        if(process_function == "fast_fam"){
            Pheno pheno;
            Marker marker;
            Geno geno(&pheno, &marker);
            FastFAM ffam(&geno);

            LOGGER.i(0, "Running fastFAM...");
            Eigen::setNbThreads(1);
            callBacks.push_back(bind(&Geno::freq, &geno, _1, _2));
            callBacks.push_back(bind(&FastFAM::calculate_fam, &ffam, _1, _2));
            geno.loop_block(callBacks);

            ffam.output(options["out"]);
        }
    }
}


