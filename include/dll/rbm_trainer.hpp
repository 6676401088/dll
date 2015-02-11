//=======================================================================
// Copyright (c) 2014-2015 Baptiste Wicht
// Distributed under the terms of the MIT License.
// (See accompanying file LICENSE or copy at
//  http://opensource.org/licenses/MIT)
//=======================================================================

#ifndef DLL_RBM_TRAINER_HPP
#define DLL_RBM_TRAINER_HPP

#include <memory>

#include "cpp_utils/algorithm.hpp"

#include "decay_type.hpp"
#include "batch.hpp"
#include "rbm_traits.hpp"

namespace dll {

enum class init_watcher_t { INIT };
constexpr const init_watcher_t init_watcher = init_watcher_t::INIT;

template<typename RBM, typename RW, typename Enable = void>
struct watcher_type {
    using watcher_t = typename RBM::desc::template watcher_t<RBM>;
};

template<typename RBM, typename RW>
struct watcher_type<RBM, RW, std::enable_if_t<cpp::not_u<std::is_void<RW>::value>::value>> {
    using watcher_t = RW;
};

/*!
 * \brief A generic trainer for Restricted Boltzmann Machine
 *
 * This trainer use the specified trainer of the RBM to perform unsupervised
 * training.
 */
template<typename RBM, bool EnableWatcher, typename RW>
struct rbm_trainer {
    using rbm_t = RBM;

    template<typename R>
    using trainer_t = typename rbm_t::desc::template trainer_t<R>;

    using watcher_t = typename watcher_type<rbm_t, RW>::watcher_t;

    mutable watcher_t watcher;

    rbm_trainer() : watcher() {}

    template<typename... Arg>
    rbm_trainer(init_watcher_t /*init*/, Arg... args) : watcher(args...) {}

    template<typename Iterator, typename R = RBM, cpp::enable_if_u<rbm_traits<R>::init_weights()> = cpp::detail::dummy>
    static void init_weights(RBM& rbm, Iterator first, Iterator last){
        rbm.init_weights(first, last);
    }

    template<typename Iterator, typename R = RBM, cpp::disable_if_u<rbm_traits<R>::init_weights()> = cpp::detail::dummy>
    static void init_weights(RBM&, Iterator, Iterator){
        //NOP
    }

    template<typename Iterator>
    typename rbm_t::weight train(RBM& rbm, Iterator first, Iterator last, std::size_t max_epochs) const {
        return train<false>(rbm, first, last, first, last, max_epochs);
    }

    template<bool Denoising, typename R = RBM, typename IIterator, typename EIterator>
    static std::enable_if_t<rbm_traits<R>::has_shuffle()> shuffle(IIterator ifirst, IIterator ilast, EIterator efirst, EIterator elast){
        static std::random_device rd;
        static std::mt19937_64 g(rd());

        if(Denoising){
            cpp::parallel_shuffle(ifirst, ilast, efirst, elast, g);
        } else {
            std::shuffle(ifirst, ilast, g);
        }
    }

    template<bool Denoising, typename R = RBM, typename IIterator, typename EIterator>
    static cpp::disable_if_t<rbm_traits<R>::has_shuffle()> shuffle(IIterator, IIterator, EIterator, EIterator){}

    template<bool Denoising, typename R = RBM, typename IIterator, typename EIterator, typename IVector, typename EVector, cpp::enable_if_u<rbm_traits<R>::has_shuffle()> = cpp::detail::dummy>
    static auto prepare_it(IIterator ifirst, IIterator ilast, EIterator efirst, EIterator elast, IVector& ivec, EVector& evec){
        std::copy(ifirst, ilast, std::back_inserter(ivec));

        auto input_first = ivec.begin();
        auto input_last = ivec.end();

        if(Denoising){
            std::copy(efirst, elast, std::back_inserter(evec));

            auto expected_first = evec.begin();
            auto expected_last = evec.end();
            return std::make_tuple(input_first, input_last, expected_first, expected_last);
        } else {
            return std::make_tuple(input_first, input_last, input_first, input_last);
        }
    }

    template<bool Denoising, typename R = RBM, typename IIterator, typename EIterator, typename IVector, typename EVector, cpp::disable_if_u<rbm_traits<R>::has_shuffle()> = cpp::detail::dummy>
    static auto prepare_it(IIterator ifirst, IIterator ilast, EIterator efirst, EIterator elast, IVector&, EVector&){
        return std::make_tuple(ifirst, ilast, efirst, elast);
    }

    template<typename rbm_t, typename Iterator>
    using fix_iterator_t = std::conditional_t<
        rbm_traits<rbm_t>::has_shuffle(),
        typename std::vector<typename std::iterator_traits<Iterator>::value_type>::iterator,
        Iterator>;

    template<bool Denoising = true, typename IIterator, typename EIterator>
    typename rbm_t::weight train(RBM& rbm, IIterator ifirst, IIterator ilast, EIterator efirst, EIterator elast, std::size_t max_epochs) const {
        rbm.momentum = rbm.initial_momentum;

        if(EnableWatcher){
            watcher.training_begin(rbm);
        }

        using input_iterator_t = fix_iterator_t<rbm_t, IIterator>;
        using expected_iterator_t = fix_iterator_t<rbm_t, EIterator>;

        input_iterator_t input_first;
        input_iterator_t input_last;

        expected_iterator_t expected_first;
        expected_iterator_t expected_last;

        //In case of shuffle, we don't want to shuffle the input, therefore create a copy and shuffle it

        std::vector<typename std::iterator_traits<IIterator>::value_type> input_copy;
        std::vector<typename std::iterator_traits<EIterator>::value_type> expected_copy;

        std::tie(input_first, input_last, expected_first, expected_last) = prepare_it<Denoising>(ifirst, ilast, efirst, elast, input_copy, expected_copy);

        //Some RBM may init weights based on the training data
        init_weights(rbm, input_first, input_last);

        auto trainer = std::make_unique<trainer_t<rbm_t>>(rbm);

        //Compute the number of batches
        auto batch_size = get_batch_size(rbm);

        //TODO Better handling of incomplete batch size would solve this problem (this could be done by
        //cleaning the data before the last batch)
        if(distance(input_first, input_last) % batch_size != 0){
            std::cout << "WARNING: The number of samples should be divisible by the batch size" << std::endl;
            std::cout << "         This may cause discrepancies in the results." << std::endl;
        }

        typename rbm_t::weight last_error = 0.0;

        //Train for max_epochs epoch
        for(std::size_t epoch= 0; epoch < max_epochs; ++epoch){
            //Shuffle if necessary
            shuffle<Denoising>(input_first, input_last, expected_first, expected_last);

            std::size_t batches = 0;
            std::size_t samples = 0;

            auto iit = input_first;
            auto eit = expected_first;
            auto end = input_last;

            //Create a new context for this epoch
            rbm_training_context context;

            while(iit != end){
                auto istart = iit;
                auto estart = eit;

                std::size_t i = 0;
                while(iit != end && i < batch_size){
                    ++iit;
                    ++eit;
                    ++samples;
                    ++i;
                }

                ++batches;

                auto input_batch = make_batch(istart, iit);
                auto expected_batch = make_batch(estart, eit);
                trainer->train_batch(input_batch, expected_batch, context);

                if(EnableWatcher && rbm_traits<rbm_t>::free_energy()){
                    for(auto& v : input_batch){
                        context.free_energy += rbm.free_energy(v);
                    }
                }
            }

            //Average all the gathered information
            context.reconstruction_error /= batches;
            context.sparsity /= batches;
            context.free_energy /= samples;

            //After some time increase the momentum
            if(rbm_traits<rbm_t>::has_momentum() && epoch == rbm.final_momentum_epoch){
                rbm.momentum = rbm.final_momentum;
            }

            //Notify the watcher
            if(EnableWatcher){
                watcher.epoch_end(epoch, context, rbm);
            }

            //Save the error for the return value
            last_error = context.reconstruction_error;
        }

        if(EnableWatcher){
            watcher.training_end(rbm);
        }

        return last_error;
    }
};

} //end of dll namespace

#endif
